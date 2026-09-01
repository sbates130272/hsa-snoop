#!/usr/bin/env bash
# Shared helpers for the hsa-snoop Slurm validation harness.
# Sourced by probe.sh, node-job.sh and submit-all.sh.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export REPO_ROOT

# A plain Ubuntu base with the node's own /opt/rocm bind-mounted in, rather
# than a rocm/dev-* image. Three reasons: the dev images are ~30 GB and would
# have to be pulled onto every node; the node's native ROCm is guaranteed to
# match its kernel driver; and it removes the need for a per-arch image map
# (gfx906/gfx908 are dropped by ROCm 7.x, gfx950 needs 7.x). Verified building
# hsa-snoop plus the HIP examples on hpe-rack-02 (gfx90a, ROCm 7.2.0).
BASE_IMAGE="${HSA_SNOOP_BASE_IMAGE:-ubuntu:24.04}"
export BASE_IMAGE

# Packages the build needs on top of the bare base image. hipcc itself comes
# from the mounted /opt/rocm; these are its shared-library dependencies.
# libdrm-amdgpu1 is required separately from libdrm2: the HIP runtime dlopens
# libdrm_amdgpu.so.1, and without it every HIP binary dies at exec time.
CONTAINER_PKGS="cmake g++ git python3 pciutils \
libnuma1 libelf1 libzstd1 libgomp1 libtinfo6 \
libdrm2 libdrm-amdgpu1"
export CONTAINER_PKGS

# Normalise a Slurm AVAIL_FEATURES / rocm-smi string to a bare gfx target.
normalise_arch() {
    local raw="${1,,}"
    raw="${raw%%:*}"
    # Strip marketing suffixes: gfx942-mi300x -> gfx942, gfx1100w -> gfx1100.
    raw="${raw%%-*}"
    case "$raw" in
        gfx1030v | gfx1100p | gfx1100w | gfx1101v) raw="${raw%?}" ;;
    esac
    echo "$raw"
}

# Detect the GFX target of the node we are running on.
detect_arch() {
    local arch=""
    if command -v rocminfo >/dev/null 2>&1; then
        arch=$(rocminfo 2>/dev/null | grep -om1 'gfx[0-9a-f]\+' || true)
    fi
    if [[ -z $arch ]] && command -v rocm_agent_enumerator >/dev/null 2>&1; then
        arch=$(rocm_agent_enumerator 2>/dev/null | grep -vm1 gfx000 || true)
    fi
    if [[ -z $arch ]] && command -v rocm-smi >/dev/null 2>&1; then
        arch=$(rocm-smi --showproductname 2>/dev/null |
            grep -oim1 'gfx[0-9a-f]\+' || true)
    fi
    [[ -n $arch ]] && normalise_arch "$arch"
}

# Run a command as root in a privileged container with the KFD, the DRI render
# nodes, tracefs and the host ROCm install mapped through. Compute nodes have
# neither passwordless sudo nor cmake, so this is the default execution context.
run_priv() {
    # --pid host is required, not cosmetic: hsa-snoop matches queues to its
    # child by PID, but ftrace reports PIDs in the init namespace. In a private
    # PID namespace the two never match and every run reports 0 queues even
    # though the workload dispatches normally.
    docker run --rm --privileged \
        --device /dev/kfd --device /dev/dri \
        --network host --ipc host --pid host \
        -v /sys/kernel/tracing:/sys/kernel/tracing \
        -v /opt:/opt:ro \
        -v /etc/alternatives:/etc/alternatives:ro \
        -v "$REPO_ROOT:$REPO_ROOT" \
        -v "$RESULTS_DIR:$RESULTS_DIR" \
        -w "$REPO_ROOT" \
        -e HOME=/tmp \
        -e PATH=/opt/rocm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        -e LD_LIBRARY_PATH=/opt/rocm/lib \
        "$BASE_IMAGE" bash -c "$*"
}

log() { printf '[%s] %s\n' "$(date -Is)" "$*"; }
