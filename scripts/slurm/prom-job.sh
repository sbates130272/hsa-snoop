#!/usr/bin/env bash
# End-to-end test of hsa-snoop's Prometheus exporter against a REAL Prometheus
# server, not just a curl of /metrics.
#
# Why a real server: it validates the exposition format as Prometheus itself
# judges it, and it produces a time series rather than one sample, which is the
# only way to check the counter contract (monotonicity) and to observe the
# kernarg-recycling race corrupting hsa_kernel_mapped_vram_bytes_total.
#
# Emits RAW ARTEFACTS ONLY into $RESULTS_DIR/<node>-<arch>/prom/. All pass/fail
# analysis happens in prom_report.py on the login node.
#
# Usage: RESULTS_DIR=<dir> scripts/slurm/prom-job.sh

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

# Seconds to let Prometheus scrape. At the 1s scrape interval configured below
# this yields ~75 samples per series, enough for a meaningful monotonicity check.
WINDOW="${HSA_SNOOP_PROM_WINDOW:-75}"
# 9488 is hsa-snoop's documented exporter port (rocm-aic scrapes it there).
PROM_PORT=9488
PROM_SRV_PORT=9490 # not 9090: avoid colliding with anything already on the node

NODE="$(hostname -s)"
ARCH="$(detect_arch || true)"
[[ -z ${ARCH:-} ]] && ARCH="unknown"
OUT="$RESULTS_DIR/${NODE}-${ARCH}/prom"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/job.log") 2>&1
log "prom-job node=$NODE arch=$ARCH window=${WINDOW}s"

if [[ $ARCH == unknown ]]; then
    log "FATAL: could not determine GFX target"
    echo "arch detection failed" >"$OUT/ABORTED"
    exit 1
fi

BUILD_DIR="build-prom/$ARCH"
N_ELEMS=1048576
{
    echo "arch=$ARCH"
    echo "node=$NODE"
    echo "n_elems=$N_ELEMS"
    echo "window=$WINDOW"
} >"$OUT/params.txt"

# A cancelled run can leave its exporter container alive, still holding the
# build tree open over NFS. Sweep those before starting.
timeout 120 bash -c 'docker ps -aq --filter "name=hsasnoop-prom-" | xargs -r docker rm -f' >/dev/null 2>&1
timeout 120 bash -c 'docker ps -aq --filter "name=prometheus-" | xargs -r docker rm -f' >/dev/null 2>&1

CNAME="hsasnoop-prom-$$"
PNAME="prometheus-$$"
# Every docker call here is wrapped in `timeout`. Tearing down a --privileged
# --pid host container whose child is still dispatching can block indefinitely,
# and because this runs from an EXIT trap a hang leaves the Slurm allocation
# held long after the results have been written.
cleanup() {
    timeout 60 docker rm -f "$CNAME" >/dev/null 2>&1
    timeout 60 docker rm -f "$PNAME" >/dev/null 2>&1
}
trap cleanup EXIT

###############################################################################
# Stage 1 — build with the exporter enabled
###############################################################################
# The exporter is OFF by default, so nothing in the standard matrix ever
# compiles this code, let alone runs it. FetchContent pulls prometheus-cpp
# v1.2.4 from GitHub, which needs git + ca-certificates in the container.
cat >"$OUT/build.sh" <<EOF
set -euo pipefail
$(apt_prelude)
cd "$REPO_ROOT"
# NFS leaves .nfsXXXX silly-rename files behind when a previous run's container
# still holds a binary open, so rm -rf fails with "Directory not empty". That
# must not abort the build: clearing the CMake cache is sufficient, because the
# stale entry that actually matters is hip_FOUND.
rm -rf "$BUILD_DIR" 2>/dev/null \
  || rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles" 2>/dev/null \
  || true
cmake -B "$BUILD_DIR" \\
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
    -DHSA_SNOOP_PROMETHEUS=ON \\
    -DGPU_TARGETS=$ARCH
cmake --build "$BUILD_DIR" --parallel \$(nproc)
# Confirm the exporter really compiled in. Capture to a variable rather than
# piping to 'grep -q': grep exits at the first match and closes the pipe, so
# hsa-snoop dies of SIGPIPE (141) and 'set -o pipefail' fails the build even
# though it succeeded.
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

# The exporter is useless without a workload to observe. CMake skips examples/
# entirely when it cannot find HIP, and that must not look like a passing run.
if [[ ! -x $REPO_ROOT/$BUILD_DIR/examples/gfx-test ]]; then
    log "FATAL: $BUILD_DIR/examples/gfx-test missing (HIP not found at configure time?)"
    grep -i "ROCm HIP" "$OUT/build.log" | tail -2
    echo "gfx-test not built" >"$OUT/ABORTED"
    touch "$OUT/DONE"
    exit 0
fi

###############################################################################
# Stage 2 — trace-mode reference run
###############################################################################
# Prometheus mode and trace mode are mutually exclusive in main.cpp (the [mem]
# fprintf is on the else branch), so the cross-mode comparison needs a separate
# trace run over the same workload.
cat >"$OUT/trace.sh" <<EOF
set -uo pipefail
$(apt_prelude)
cd "$REPO_ROOT"
"$BUILD_DIR/hsa-snoop" --mem-snoop --format json --out /tmp/prom-ref.json \\
    -- "$BUILD_DIR/examples/gfx-test" -n $N_ELEMS -b 4 -l 10 -s 100
EOF
log "stage 2: trace-mode reference run"
run_priv "bash $OUT/trace.sh" >"$OUT/trace-mode.log" 2>&1

###############################################################################
# Stage 3 — serve metrics and scrape them with a real Prometheus
###############################################################################
cat >"$OUT/prometheus.yml" <<EOF
global:
  scrape_interval: 1s
  evaluation_interval: 1s
scrape_configs:
  - job_name: hsa-snoop
    static_configs:
      - targets: ['localhost:$PROM_PORT']
EOF

cat >"$OUT/serve.sh" <<EOF
set -uo pipefail
$(apt_prelude)
cd "$REPO_ROOT"
exec "$BUILD_DIR/hsa-snoop" --mem-snoop --prometheus --prometheus-port $PROM_PORT \\
    -- "$BUILD_DIR/examples/gfx-test" -n $N_ELEMS -b 4 -l 100000 -s 100
EOF

log "stage 3: starting hsa-snoop exporter (detached)"
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
# scrapes are not spurious failures.
# Probe with the NODE's bash, not a container: the containers run with
# --network host anyway, so 127.0.0.1 is the same stack, and spawning a
# container per poll costs seconds each.
#
# Use `exec 3<>` rather than `cat < /dev/tcp/...`: cat blocks reading from a
# SUCCESSFULLY connected socket (the exporter sends nothing until it gets an
# HTTP request), so `timeout 3 cat` kills it and returns 124 — making a healthy
# exporter look unreachable and burning the whole retry budget.
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
    docker logs "$CNAME" >"$OUT/exporter-container.log" 2>&1
    echo "exporter never listened" >"$OUT/ABORTED"
    touch "$OUT/DONE"
    exit 0
fi

mkdir -p "$OUT/tsdb"
chmod 777 "$OUT/tsdb"
log "stage 3: starting prometheus server"
docker run -d --name "$PNAME" --network host --user root \
    -v "$OUT/prometheus.yml:/etc/prometheus/prometheus.yml:ro" \
    -v "$OUT/tsdb:/prometheus" \
    "${HSA_SNOOP_PROM_IMAGE:-prom/prometheus:v3.13.2}" \
    --config.file=/etc/prometheus/prometheus.yml \
    --storage.tsdb.path=/prometheus \
    --storage.tsdb.retention.time=2h \
    --web.listen-address="0.0.0.0:$PROM_SRV_PORT" >/dev/null 2>&1

START=$(date +%s)
log "scraping for ${WINDOW}s"
sleep "$WINDOW"
END=$(date +%s)

# A direct scrape too, so the raw exposition text is kept as an artefact.
docker run --rm --network host "$BASE_IMAGE" bash -c \
    "apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq curl >/dev/null 2>&1;
     curl -s --max-time 10 localhost:$PROM_PORT/metrics" >"$OUT/metrics-raw.txt" 2>/dev/null

###############################################################################
# Stage 4 — query the TSDB
###############################################################################
QUERIES="hsa_kernel_lds_bytes hsa_kernel_scratch_bytes_total hsa_kernel_mapped_vram_bytes hsa_kernel_mapped_vram_bytes_total"
cat >"$OUT/query.sh" <<EOF
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq curl >/dev/null 2>&1
for q in $QUERIES; do
  curl -sG --max-time 30 "http://localhost:$PROM_SRV_PORT/api/v1/query_range" \\
    --data-urlencode "query=\$q" \\
    --data-urlencode "start=$START" \\
    --data-urlencode "end=$END" \\
    --data-urlencode "step=1s" > "$OUT/range-\$q.json"
  # Prometheus's own view of each metric's TYPE — objective evidence for the
  # gauge-named-_total question, straight from the server that ingested it.
  curl -sG --max-time 15 "http://localhost:$PROM_SRV_PORT/api/v1/metadata" \\
    --data-urlencode "metric=\$q" > "$OUT/metadata-\$q.json"
done
curl -sG --max-time 15 "http://localhost:$PROM_SRV_PORT/api/v1/query" \\
  --data-urlencode "query=up{job=\"hsa-snoop\"}" > "$OUT/up.json"
EOF
log "stage 4: querying the TSDB"
docker run --rm --network host -v "$RESULTS_DIR:$RESULTS_DIR" "$BASE_IMAGE" \
    bash "$OUT/query.sh" >"$OUT/query.log" 2>&1

timeout 60 docker logs "$CNAME" >"$OUT/exporter-container.log" 2>&1
timeout 60 docker logs "$PNAME" >"$OUT/prometheus-container.log" 2>&1
cleanup

log "prom-job complete"
touch "$OUT/DONE"
