#!/usr/bin/env bash
# Per-node body of the hsa-snoop multi-GFX validation run.
#
# Emits RAW LOGS ONLY into $RESULTS_DIR/<node>-<arch>/. All pass/fail analysis
# happens later in report.py on the login node, so the oracles can be fixed and
# re-evaluated without re-running the GPU jobs.
#
# Usage: RESULTS_DIR=<dir> scripts/slurm/node-job.sh

set -uo pipefail

# sbatch copies the batch script into /var/spool/slurmd/job<N>/slurm_script, so
# BASH_SOURCE points at the spool copy and lib.sh is not beside it. Prefer the
# repo path exported by submit-all.sh, and fall back to BASH_SOURCE for direct
# srun / manual invocation, where the script does run in place.
_here="$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
HSA_SNOOP_SCRIPTS="${HSA_SNOOP_REPO:+$HSA_SNOOP_REPO/scripts/slurm}"
if [[ -z ${HSA_SNOOP_SCRIPTS:-} || ! -f $HSA_SNOOP_SCRIPTS/lib.sh ]]; then
    HSA_SNOOP_SCRIPTS="$_here"
fi
if [[ ! -f $HSA_SNOOP_SCRIPTS/lib.sh ]]; then
    echo "FATAL: cannot locate lib.sh (set HSA_SNOOP_REPO to the repo root)" >&2
    exit 1
fi
# shellcheck source=lib.sh
source "$HSA_SNOOP_SCRIPTS/lib.sh"

# lib.sh turns on -e; this driver must keep running past a failing stage so a
# single broken arch still produces logs for every other stage.
set +e

: "${RESULTS_DIR:?RESULTS_DIR must be set}"

NODE="$(hostname -s)"
ARCH="$(detect_arch || true)"
[[ -z ${ARCH:-} ]] && ARCH="unknown"
OUT="$RESULTS_DIR/${NODE}-${ARCH}"
mkdir -p "$OUT"

exec > >(tee -a "$OUT/job.log") 2>&1
log "node=$NODE arch=$ARCH job=${SLURM_JOB_ID:-none} partition=${SLURM_JOB_PARTITION:-none}"

###############################################################################
# Stage 0 — capability probe (native, outside the container)
###############################################################################
{
    echo "hostname=$(hostname -f)"
    echo "node=$NODE"
    echo "arch=$ARCH"
    echo "partition=${SLURM_JOB_PARTITION:-none}"
    echo "slurm_job=${SLURM_JOB_ID:-none}"
    echo "kernel=$(uname -r)"
    echo "date=$(date -Is)"
    echo "id=$(id)"

    if sudo -n true 2>/dev/null; then
        echo "sudo_nopasswd=yes"
        echo "--- sudo -n -l ---"
        sudo -n -l 2>&1 | head -30
    else
        echo "sudo_nopasswd=no"
    fi

    if [[ -r /sys/kernel/tracing/kprobe_events ]]; then
        echo "tracefs_readable=yes"
    else
        echo "tracefs_readable=no"
    fi

    if command -v docker >/dev/null 2>&1; then
        echo "docker=yes server=$(timeout 30 docker info --format '{{.ServerVersion}}' 2>&1 | head -1)"
    else
        echo "docker=no"
    fi

    echo "cmake_native=$(command -v cmake || echo none)"
    echo "rocm_dirs=$(ls -d /opt/rocm* 2>/dev/null | tr '\n' ' ')"
    echo "rocm_real=$(readlink -f /opt/rocm 2>/dev/null)"
    echo "rocm_version=$(cat /opt/rocm/.info/version 2>/dev/null)"
    # Some nodes advertise a ROCM* Slurm feature but carry only an empty
    # /opt/rocm stub, so check for the HIP CMake package rather than the
    # directory. Without it, find_package(hip) fails and the build cannot run.
    if [[ -f /opt/rocm/lib/cmake/hip/hip-config.cmake ]]; then
        echo "rocm_usable=yes"
    else
        echo "rocm_usable=no"
    fi
    echo "kfd_create_queue_ksym=$(grep -c kfd_ioctl_create_queue /proc/kallsyms 2>/dev/null || echo 0)"
    echo "--- rocm-smi ---"
    timeout 60 rocm-smi --showproductname 2>&1 | head -40
} >"$OUT/probe.txt" 2>&1
log "stage 0 probe written"

if [[ $ARCH == unknown ]]; then
    log "FATAL: could not determine GFX target; aborting before build"
    echo "arch detection failed" >"$OUT/ABORTED"
    exit 1
fi

if [[ ! -f /opt/rocm/lib/cmake/hip/hip-config.cmake ]]; then
    log "SKIP: node has no usable ROCm HIP install (/opt/rocm is a stub)"
    echo "no usable ROCm HIP install on this node" >"$OUT/NO_ROCM"
    touch "$OUT/DONE"
    exit 0
fi

# Per-NODE, not per-arch: the watcher retries an arch on a different node while
# an earlier job may still be running, and the repo is shared over NFS, so two
# jobs for the same arch would otherwise rm -rf and rebuild the same directory
# underneath each other.
BUILD_DIR="build-slurm/$ARCH-$NODE"
N_ELEMS=1048576
log "image=$BASE_IMAGE build_dir=$BUILD_DIR"
{
    echo "image=$BASE_IMAGE"
    echo "build_dir=$BUILD_DIR"
    echo "n_elems=$N_ELEMS"
    echo "arch=$ARCH"
    echo "node=$NODE"
} >"$OUT/params.txt"

###############################################################################
# Stages 1 and 2 — build and test
###############################################################################
# All stages run in ONE container invocation. A container per stage was tried
# first and fails: the apt-installed toolchain (ctest, libelf, python3) lives in
# the container's writable layer and is destroyed with it, so later stages come
# up bare. The body is written to a file under $OUT (bind-mounted) rather than
# passed as a quoted string, so nothing has to survive two levels of quoting.
cat >"$OUT/stages.sh" <<EOF
set -uo pipefail
export DEBIAN_FRONTEND=noninteractive
cd "$REPO_ROOT"

{
    apt-get update -qq
    apt-get install -y -qq $CONTAINER_PKGS >/dev/null
    cmake --version
    hipcc --version 2>&1 | head -2
    echo "rocm_version=\$(cat /opt/rocm/.info/version 2>/dev/null)"
    rm -rf "$BUILD_DIR"
    cmake -B "$BUILD_DIR" \\
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \\
        -DBUILD_TESTING=ON \\
        -DHSA_SNOOP_HARDWARE_TESTS=ON \\
        -DGPU_TARGETS=$ARCH
    cmake --build "$BUILD_DIR" --parallel \$(nproc)
    ls -l "$BUILD_DIR/hsa-snoop" "$BUILD_DIR/examples/gfx-test" "$BUILD_DIR/examples/sdma-test"
} >"$OUT/build.log" 2>&1
BUILD_RC=\$?
echo "\$BUILD_RC" >"$OUT/build.rc"
if [[ \$BUILD_RC -ne 0 ]]; then
    echo "build failed rc=\$BUILD_RC; skipping tests"
    exit 0
fi

{
    echo "===== (a) unit: sdma-ring-test ====="
    timeout 300 ctest --test-dir "$BUILD_DIR" -R sdma-ring-test --output-on-failure
    echo "unit_rc=\$?"
    echo "===== (b) hardware: sdma-hardware-test ====="
    timeout 600 ctest --test-dir "$BUILD_DIR" -L hardware --output-on-failure
    echo "hw_rc=\$?"
} >"$OUT/ctest.log" 2>&1

# --mem-snoop against gfx-test, whose buffer sizes and __shared__ arrays are
# known exactly (examples/gfx-test.hip) and serve as the oracle.
{
    echo "===== gfx-test standalone sanity ====="
    timeout 180 "$BUILD_DIR/examples/gfx-test" -n $N_ELEMS -b 2 -l 1 -s 50
    echo "gfx_test_rc=\$?"

    echo "===== rocm-smi vram ====="
    timeout 60 rocm-smi --showmeminfo vram 2>&1 | head -30

    echo "===== hsa-snoop --mem-snoop ====="
    timeout 300 "$BUILD_DIR/hsa-snoop" --mem-snoop \\
        --format json --out "$OUT/mem-snoop-trace.json" \\
        -- "$BUILD_DIR/examples/gfx-test" -n $N_ELEMS -b 4 -l 3 -s 50
    echo "mem_snoop_rc=\$?"

    echo "===== hsa-snoop --help (flag presence) ====="
    "$BUILD_DIR/hsa-snoop" --help 2>&1 | grep -A3 mem-snoop
} >"$OUT/mem-snoop.log" 2>&1
EOF

log "container stages starting"
run_priv "bash $OUT/stages.sh" >"$OUT/container.log" 2>&1
log "container stages finished rc=$?"

log "node job complete"
touch "$OUT/DONE"
