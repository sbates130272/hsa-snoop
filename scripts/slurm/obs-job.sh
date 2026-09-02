#!/usr/bin/env bash
# Capture a REPLAYABLE observability dataset from a real GPU node: a Prometheus
# TSDB snapshot of hsa-snoop's exporter plus the exporter's own stderr log.
#
# Why a snapshot and not a live stack: iterating on a Grafana dashboard takes
# many minutes of clicking, and holding a --gres=gpu:1 allocation open that long
# to stare at a graph is wasteful. This job runs the workload once, freezes both
# the metrics and the logs into $RESULTS_DIR, and releases the node. The login
# node then replays them offline with scripts/observability/stack.sh.
#
# Unlike prom-job.sh (which exists to VALIDATE the exposition format), this job
# exists to PRODUCE DATA WORTH LOOKING AT: hsa-snoop runs in --all mode and the
# workload moves through distinct phases, so the dashboard shows a changing
# kernel mix, an SDMA burst and idle gaps rather than one flat line.
#
# Emits raw artefacts only into $RESULTS_DIR/<node>-<arch>/obs/.
#
# Usage: RESULTS_DIR=<dir> scripts/slurm/obs-job.sh

set -uo pipefail

_here="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
HSA_SNOOP_SCRIPTS="${HSA_SNOOP_REPO:+$HSA_SNOOP_REPO/scripts/slurm}"
if [[ -z ${HSA_SNOOP_SCRIPTS:-} || ! -f $HSA_SNOOP_SCRIPTS/lib.sh ]]; then
    HSA_SNOOP_SCRIPTS="$_here"
fi
# shellcheck source=lib.sh
source "$HSA_SNOOP_SCRIPTS/lib.sh"
set +e

: "${RESULTS_DIR:?RESULTS_DIR must be set}"

PROM_PORT=9488
PROM_SRV_PORT=9490 # not 9090: avoid colliding with anything already on the node

NODE="$(hostname -s)"
ARCH="$(detect_arch || true)"
[[ -z ${ARCH:-} ]] && ARCH="unknown"
OUT="$RESULTS_DIR/${NODE}-${ARCH}/obs"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/job.log") 2>&1
log "obs-job node=$NODE arch=$ARCH"

if [[ $ARCH == unknown ]]; then
    log "FATAL: could not determine GFX target"
    echo "arch detection failed" >"$OUT/ABORTED"
    exit 1
fi

BUILD_DIR="build-obs/$ARCH"
{
    echo "arch=$ARCH"
    echo "node=$NODE"
    echo "started=$(date -Is)"
} >"$OUT/params.txt"

# AIS needs a file on an NVMe-backed filesystem that hsa-snoop and the workload
# can both reach. /var/tmp, not the container overlay and not $RESULTS_DIR:
# get_pci_dev_from_file() walks the VFS mount/superblock chain to find the
# backing PCIe device, and neither overlayfs nor NFS leads anywhere.
AIS_DIR="${HSA_SNOOP_AIS_DIR:-/var/tmp/hsa-snoop-ais}"
AIS_FILE="$AIS_DIR/ais-test.bin"
export HSA_SNOOP_EXTRA_MOUNTS="-v /var/tmp:/var/tmp"

###############################################################################
# Stage 0 — record whether AIS can possibly work here
###############################################################################
# --ais-snoop is now always on, so the AIS row of the dashboard is always
# *queried*. Whether it can have data depends on the node, and an empty row
# with no explanation is the most confusing possible outcome. Write the verdict
# next to the capture.
mkdir -p "$AIS_DIR" 2>/dev/null || true
# No '$' anchor: /proc/kallsyms lines are "<addr> t kfd_ioctl_ais\t[amdgpu]", so
# anchoring the symbol name to end-of-line never matches and reported 0 on a node
# that plainly had the symbol. And `grep -c ... || echo 0` prints TWICE when
# there are no matches -- grep -c emits "0" and still exits 1 -- so the field
# came out as two lines. `|| true` is what was meant.
{
    echo "kfd_ioctl_ais_symbol=$(grep -c 'kfd_ioctl_ais' /proc/kallsyms 2>/dev/null || true)"
    echo "amdgpu_version=$(modinfo amdgpu 2>/dev/null | awk '/^version:/{print $2; exit}')"
    echo "nvme_devices=$(lsblk -d -o NAME,TRAN 2>/dev/null | grep -c nvme || true)"
    echo "ais_dir=$AIS_DIR"
    echo "ais_dir_backing=$(df --output=source "$AIS_DIR" 2>/dev/null | tail -1)"
    echo "ais_dir_on_nvme=$(df --output=source "$AIS_DIR" 2>/dev/null | tail -1 | grep -qi nvme && echo yes || echo no)"
} >"$OUT/ais-support.txt"
log "AIS support probe:"
sed 's/^/    /' "$OUT/ais-support.txt"

###############################################################################
# Stage 0b — preflight the ROCm install
###############################################################################
# Several nodes advertise a ROCM feature but carry an incomplete /opt/rocm. On
# ctr-cx63-mi300x-12 hipcc is present but lib/cmake/hip/ is not, so
# find_package(hip) fails, CMake silently skips examples/ and the only clue is
# one line of "ROCm HIP not found" buried in a 60-second build log. Check the
# file CMake actually looks for, up front, and say which node and which path.
ROCM_REAL="$(readlink -f /opt/rocm 2>/dev/null)"
[[ -d ${ROCM_REAL:-} ]] || ROCM_REAL=/opt/rocm
HIP_CONFIG=""
for c in "$ROCM_REAL/lib/cmake/hip/hip-config.cmake" \
         "$ROCM_REAL/lib/cmake/hip/hip-config-version.cmake"; do
    [[ -f $c ]] && { HIP_CONFIG="$c"; break; }
done
if [[ -z $HIP_CONFIG ]]; then
    log "FATAL: $NODE has an incomplete ROCm: no hip-config.cmake under"
    log "       $ROCM_REAL/lib/cmake/hip/ (hipcc present: $([[ -x $ROCM_REAL/bin/hipcc ]] && echo yes || echo no))"
    log "       find_package(hip) would fail and examples/ would be skipped."
    {
        echo "node=$NODE"
        echo "rocm_real=$ROCM_REAL"
        echo "hipcc=$([[ -x $ROCM_REAL/bin/hipcc ]] && echo present || echo missing)"
        echo "hip_cmake_dir=missing"
    } >"$OUT/ABORTED"
    touch "$OUT/DONE"
    exit 0
fi
log "ROCm preflight ok: $HIP_CONFIG"

timeout 120 bash -c 'docker ps -aq --filter "name=hsasnoop-obs-" | xargs -r docker rm -f' >/dev/null 2>&1
timeout 120 bash -c 'docker ps -aq --filter "name=promobs-" | xargs -r docker rm -f' >/dev/null 2>&1

CNAME="hsasnoop-obs-$$"
PNAME="promobs-$$"
# Every docker call is wrapped in `timeout`: tearing down a --privileged --pid
# host container whose child is still dispatching can block indefinitely, and
# from an EXIT trap a hang holds the Slurm allocation long after the results
# have been written.
cleanup() {
    timeout 60 docker rm -f "$CNAME" >/dev/null 2>&1
    timeout 60 docker rm -f "$PNAME" >/dev/null 2>&1
}
trap cleanup EXIT

###############################################################################
# Stage 1 — build with the exporter enabled
###############################################################################
cat >"$OUT/build.sh" <<EOF
set -euo pipefail
$(apt_prelude)
cd "$REPO_ROOT"
# NFS silly-rename files (.nfsXXXX) left by a previous run's container make
# rm -rf fail with "Directory not empty". Clearing the CMake cache is enough:
# the stale entry that actually matters is hip_FOUND.
rm -rf "$BUILD_DIR" 2>/dev/null \
  || rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles" 2>/dev/null \
  || true
cmake -B "$BUILD_DIR" \\
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
    -DHSA_SNOOP_PROMETHEUS=ON \\
    -DGPU_TARGETS=$ARCH
cmake --build "$BUILD_DIR" --parallel \$(nproc)
# Capture to a variable rather than piping to 'grep -q': grep exits at the first
# match and closes the pipe, so hsa-snoop dies of SIGPIPE (141) and pipefail
# fails a build that actually succeeded.
help_out="\$("$BUILD_DIR/hsa-snoop" --help 2>&1)"
case "\$help_out" in
    *--prometheus*) ;;
    *) echo "FATAL: binary has no --prometheus flag"; exit 1 ;;
esac
EOF

log "stage 1: building with -DHSA_SNOOP_PROMETHEUS=ON"
run_priv "bash $OUT/build.sh" >"$OUT/build.log" 2>&1
BUILD_RC=$?
echo "$BUILD_RC" >"$OUT/build.rc"
log "stage 1 finished rc=$BUILD_RC"
[[ $BUILD_RC -ne 0 ]] && { log "build failed"; touch "$OUT/DONE"; exit 0; }

# CMake skips examples/ entirely when it cannot find HIP, and that must not look
# like a passing run.
for ex in gfx-test sdma-test ais-test; do
    if [[ ! -x $REPO_ROOT/$BUILD_DIR/examples/$ex ]]; then
        log "FATAL: $BUILD_DIR/examples/$ex missing (HIP not found at configure time?)"
        grep -i "ROCm HIP" "$OUT/build.log" | tail -2
        echo "$ex not built" >"$OUT/ABORTED"
        touch "$OUT/DONE"
        exit 0
    fi
done

###############################################################################
# Stage 2 — start the exporter in --all mode
###############################################################################
# --all, not "-- <cmd>": the workload runs as a sequence of separate processes
# below, and only daemon mode sees queues it did not itself launch. This is also
# the mode hsa-snoop-prometheus.service uses, so the captured data matches what
# a real deployment would produce.
#
# HSA_SNOOP_DEBUG is deliberately NOT set. At ~100 dispatches/s the per-packet
# debug lines run to hundreds of MB and swamp the [queue] discovery lines that
# are the useful signal in Loki. --log-dispatches is the structured replacement:
# one NDJSON line per dispatch, which is what makes a real per-launch kernel
# timeline possible (a 1 s Prometheus scrape cannot distinguish the ~100 kernels
# that ran inside each sample).
#
# --ais-snoop is unconditional. It costs a forked bpftrace and one kprobe, and
# when the node has no AIS the monitor simply never fires -- whereas making it
# conditional means the AIS metrics only ever exist on captures somebody
# remembered to special-case, and the dashboard row rots. bpftrace is installed
# here rather than in CONTAINER_PKGS so the rest of the Slurm matrix does not
# pay for it.
cat >"$OUT/serve.sh" <<EOF
set -uo pipefail
$(apt_prelude)
apt-get install -y -qq bpftrace >/dev/null 2>&1
cd "$REPO_ROOT"
exec "$BUILD_DIR/hsa-snoop" --all --mem-snoop --prometheus --ais-snoop \\
    --prometheus-port $PROM_PORT --out-dir /tmp/hsa-snoop-traces \\
    --log-dispatches "$OUT/dispatches.ndjson"
EOF

log "stage 2: starting hsa-snoop --all exporter (detached)"
# Same /opt/rocm symlink resolution as run_priv() -- see the comment there.
ROCM_REAL="$(readlink -f /opt/rocm 2>/dev/null)"
[[ -d $ROCM_REAL ]] || ROCM_REAL=/opt/rocm
docker run -d --name "$CNAME" --privileged \
    --device /dev/kfd --device /dev/dri \
    --network host --ipc host --pid host \
    -v /sys/kernel/tracing:/sys/kernel/tracing \
    -v /sys/kernel/debug:/sys/kernel/debug \
    -v /opt:/opt:ro \
    -v "$ROCM_REAL:/opt/rocm:ro" \
    -v "$REPO_ROOT:$REPO_ROOT" -v "$RESULTS_DIR:$RESULTS_DIR" \
    -w "$REPO_ROOT" -e HOME=/tmp \
    -e PATH=/opt/rocm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    -e LD_LIBRARY_PATH=/opt/rocm/lib \
    "$BASE_IMAGE" bash "$OUT/serve.sh" >/dev/null 2>&1

# Wait for the exporter to bind before pointing Prometheus at it, so the first
# scrapes are not spurious failures. Probe from the node's own bash: the
# containers run --network host anyway, so 127.0.0.1 is the same stack, and
# spawning a container per poll costs seconds each.
#
# `exec 3<>` rather than `cat < /dev/tcp/...`: cat blocks reading a SUCCESSFULLY
# connected socket (the exporter sends nothing until it gets an HTTP request),
# so `timeout 3 cat` returns 124 and a healthy exporter looks unreachable.
listening=0
for i in $(seq 1 180); do
    if timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PROM_PORT" 2>/dev/null; then
        log "exporter is listening after ${i}s"
        listening=1
        break
    fi
    sleep 1
done
if [[ $listening -ne 1 ]]; then
    log "FATAL: exporter never bound port $PROM_PORT"
    docker logs "$CNAME" >"$OUT/hsa-snoop.log" 2>&1
    echo "exporter never listened" >"$OUT/ABORTED"
    touch "$OUT/DONE"
    exit 0
fi

###############################################################################
# Stage 3 — start Prometheus with the admin API enabled
###############################################################################
# 1s scrape: the exporter's launch-rate gauge uses a 10s window, so anything
# coarser aliases the phase transitions the workload below is designed to show.
cat >"$OUT/prometheus.yml" <<EOF
global:
  scrape_interval: 1s
  evaluation_interval: 1s
  external_labels:
    capture_node: $NODE
    capture_arch: $ARCH
scrape_configs:
  - job_name: hsa-snoop
    static_configs:
      - targets: ['localhost:$PROM_PORT']
EOF

mkdir -p "$OUT/tsdb"
chmod 777 "$OUT/tsdb"
log "stage 3: starting prometheus (admin API on for the snapshot)"
# --web.enable-admin-api is the whole point: /api/v1/admin/tsdb/snapshot writes
# a self-contained, hard-linked block set. Copying the live tsdb directory
# instead would capture only a WAL (short runs never compact a block), which
# replays but leaves the login-node Prometheus writable and mutating the
# artefact it is supposed to be replaying.
docker run -d --name "$PNAME" --network host --user root \
    -v "$OUT/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
    -v "$OUT/tsdb:/prometheus" \
    "${HSA_SNOOP_PROM_IMAGE:-prom/prometheus:v3.13.2}" \
    --config.file=/etc/prometheus/prometheus.yml \
    --storage.tsdb.path=/prometheus \
    --storage.tsdb.retention.time=6h \
    --web.enable-admin-api \
    --web.listen-address="0.0.0.0:$PROM_SRV_PORT" >/dev/null 2>&1

for i in $(seq 1 60); do
    timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/$PROM_SRV_PORT" 2>/dev/null && break
    sleep 1
done

###############################################################################
# Stage 4 — phased workload
###############################################################################
# Distinct phases with idle gaps between them. The gaps matter as much as the
# work: a dashboard that only ever sees saturation cannot be judged on whether
# it reads correctly at zero, and the "last kernel" / "triggered" latching
# gauges are only interesting either side of an idle period.
run_phase() {
    local name="$1" script="$2"
    log "  phase $name: start $(date -Is)"
    echo "$name start $(date +%s)" >>"$OUT/phases.txt"
    cat >"$OUT/phase-$name.sh" <<EOF
set -uo pipefail
$(apt_prelude)
cd "$REPO_ROOT"
$script
EOF
    run_priv "bash $OUT/phase-$name.sh" >"$OUT/phase-$name.log" 2>&1
    echo "$name end $(date +%s)" >>"$OUT/phases.txt"
    log "  phase $name: end $(date -Is) rc=$?"
}

: >"$OUT/phases.txt"
log "stage 4: phased workload"

sleep 15 # a visible idle baseline before the first dispatch

# Small problem, tight loop: high launch rate, short kernels.
run_phase burst \
    "timeout 120 $BUILD_DIR/examples/gfx-test -n 262144 -b 8 -l 4000 -s 5"

sleep 20

# Large problem, slow loop: low launch rate, long kernels. Together with the
# burst phase this gives the duration histogram two well-separated modes, which
# is what makes the p50/p99 panels meaningful instead of two overlapping lines.
run_phase heavy \
    "timeout 90 $BUILD_DIR/examples/gfx-test -n 16777216 -b 2 -l 200 -s 100"

sleep 20

# SDMA copy traffic. Separate binary and separate queues, so the SDMA row of
# the dashboard lights up in a window where the AQL row is quiet.
run_phase sdma \
    "timeout 90 $BUILD_DIR/examples/sdma-test -s 4 -m 256 -i 200 -r 10"

sleep 20

# AIS P2P direct storage. -s 0 draws a random power-of-two size per iteration
# between 4 KiB and 8 MiB, which is the only way the ais_*_io_size_bytes
# histogram gets more than one populated bucket; -b alternates read and write so
# both the rx and tx metric families are exercised rather than half the row.
#
# Every step is logged rather than guarded: on a node without NVMe or with
# amdgpu < 6.19 this phase produces nothing, and the reason belongs in the
# artefact next to the empty panels (see ais-support.txt).
run_phase ais \
    "mkdir -p $AIS_DIR
     dd if=/dev/urandom of=$AIS_FILE bs=1M count=512 status=none || true
     ls -l $AIS_FILE || true
     df -hT $AIS_DIR || true
     # tr: ais-test redraws a rolling stats line with \r, so without a TTY the
     # whole run lands as ONE line -- 12 MB in practice, past Loki's per-entry
     # limit and past what any log viewer will render.
     timeout 100 $BUILD_DIR/examples/ais-test -b -s 0 -D 75 $AIS_FILE | tr '\r' '\n'
     # PIPESTATUS[0], not \$?: with the tr pipe added, \$? is tr's status and
     # would report success for an ais-test that failed outright.
     echo \"ais_test_rc=\${PIPESTATUS[0]}\""

sleep 20

# Two concurrent processes: the per-pid/per-comm labels and the "Top Kernels"
# table are untestable with a single producer.
run_phase concurrent \
    "timeout 90 $BUILD_DIR/examples/gfx-test -n 1048576 -b 4 -l 900 -s 20 &
     timeout 90 $BUILD_DIR/examples/gfx-test -n 4194304 -b 2 -l 400 -s 40 &
     wait"

sleep 20 # trailing idle, so the dashboard is judged on a decaying tail too

log "stage 4 complete"

###############################################################################
# Stage 5 — freeze the artefacts
###############################################################################
# A final direct scrape keeps the raw exposition text alongside the TSDB, which
# is the only artefact that shows HELP/TYPE exactly as hsa-snoop emitted them.
docker run --rm --network host "$BASE_IMAGE" bash -c \
    "apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq curl >/dev/null 2>&1;
     curl -s --max-time 10 localhost:$PROM_PORT/metrics" >"$OUT/metrics-raw.txt" 2>/dev/null

log "stage 5: snapshotting the TSDB"
cat >"$OUT/snapshot.sh" <<EOF
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq curl >/dev/null 2>&1
curl -s -XPOST --max-time 120 \\
  "http://localhost:$PROM_SRV_PORT/api/v1/admin/tsdb/snapshot" > "$OUT/snapshot.json"
EOF
docker run --rm --network host -v "$RESULTS_DIR:$RESULTS_DIR" "$BASE_IMAGE" \
    bash "$OUT/snapshot.sh" >"$OUT/snapshot.log" 2>&1
log "snapshot: $(cat "$OUT/snapshot.json" 2>/dev/null)"

# -t gives RFC3339 timestamps per line. Without them Loki has to fall back to
# ingest time, which would stack every historical line at replay time and make
# the log panel useless next to the metrics.
timeout 60 docker logs -t "$CNAME" >"$OUT/hsa-snoop.log" 2>&1
timeout 60 docker logs -t "$PNAME" >"$OUT/prometheus.log" 2>&1
log "captured $(wc -l <"$OUT/hsa-snoop.log") hsa-snoop log lines"

{
    echo "finished=$(date -Is)"
    echo "snapshot_dir=$(python3 -c "
import json,sys
try: print(json.load(open('$OUT/snapshot.json'))['data']['name'])
except Exception: print('NONE')
" 2>/dev/null)"
} >>"$OUT/params.txt"

rm -f "$AIS_FILE" 2>/dev/null || true

cleanup
log "obs-job complete"
touch "$OUT/DONE"
