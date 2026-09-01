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
#
# Mount set follows the known-good hsa-snoop container recipe in the rocm-aic
# repo (monitoring/docker-compose.monitoring.yml, service `hsa-snoop`): both
# /sys/kernel/tracing and /sys/kernel/debug, /dev/kfd + /dev/dri, privileged,
# and host PID.
#
# On some nodes /opt/rocm is a symlink into /etc/alternatives (e.g.
# /opt/rocm -> /etc/alternatives/rocm), so bind-mounting only /opt leaves it
# dangling in the container and CMake reports "ROCm HIP not found -- skipping
# examples/". Mounting /etc/alternatives is NOT the answer:
#   - read-only breaks every apt-get install, because dpkg runs
#     update-alternatives when configuring g++ and cannot write its .dpkg-tmp
#     file ("Read-only file system", dpkg error code 1);
#   - read-write would let container dpkg mutate the HOST's alternatives on a
#     shared cluster node, which could repoint the host's own /opt/rocm.
# Instead resolve the symlink host-side and mount the real versioned directory
# over /opt/rocm. /opt stays mounted too so absolute /opt/rocm-X.Y.Z paths still
# resolve, and the container keeps its own writable /etc/alternatives.
run_priv() {
    local rocm_real
    rocm_real="$(readlink -f /opt/rocm 2>/dev/null)"
    [[ -d $rocm_real ]] || rocm_real=/opt/rocm

    # --pid host is required, not cosmetic: hsa-snoop matches queues to its
    # child by PID, but ftrace reports PIDs in the init namespace. In a private
    # PID namespace the two never match and every run reports 0 queues even
    # though the workload dispatches normally.
    docker run --rm --privileged \
        --device /dev/kfd --device /dev/dri \
        --network host --ipc host --pid host \
        -v /sys/kernel/tracing:/sys/kernel/tracing \
        -v /sys/kernel/debug:/sys/kernel/debug \
        -v /opt:/opt:ro \
        -v "$rocm_real:/opt/rocm:ro" \
        -v "$REPO_ROOT:$REPO_ROOT" \
        -v "$RESULTS_DIR:$RESULTS_DIR" \
        -w "$REPO_ROOT" \
        -e HOME=/tmp \
        -e PATH=/opt/rocm/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
        -e LD_LIBRARY_PATH=/opt/rocm/lib \
        "$BASE_IMAGE" bash -c "$*"
}

# Emit the apt install block that every generated container script must start
# with. Each run_priv() call is a FRESH container, so packages installed by one
# script are gone in the next: a build script that installs libelf and a run
# script that does not will build a binary it then cannot exec
# ("error while loading shared libraries: libelf.so.1").
apt_prelude() {
    cat <<EOF
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq $CONTAINER_PKGS ca-certificates curl >/dev/null 2>&1
EOF
}

log() { printf '[%s] %s\n' "$(date -Is)" "$*"; }
