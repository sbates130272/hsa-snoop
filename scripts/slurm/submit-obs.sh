#!/usr/bin/env bash
# Submit obs-job.sh — capture a replayable Prometheus+log dataset from one GPU
# node for offline Grafana dashboard work.
#
#   scripts/slurm/submit-obs.sh [run-id] [constraint] [nodelist]
#
# Set HSA_SNOOP_OBS_PARTITION to submit somewhere other than defq. The `storage`
# partition is the better AIS target when defq is saturated: its two GPU nodes
# carry 8 and 16 NVMe drives against the 3-4 on a defq MI300X, and its queue is
# far shorter. Its third node (ctr-smc-strg-cx68-3) is CPUONLY, which the
# GFX942 half of the constraint filters out.
#
# Default constraint: GFX942&MARKHAM&NVME.
#
#   MARKHAM — AUSTIN nodes do not mount this /home, and jobs landing there die
#             before writing even their Slurm output file.
#   NVME    — obs-job.sh now always runs --ais-snoop, and AIS is P2P DMA between
#             an NVMe device and VRAM. Without a local NVMe there is nothing for
#             the AIS half of the dashboard to show. (Verified: hpe-rack-02, the
#             gfx90a node used previously, has only SATA and amdgpu 6.16 — the
#             kfd_ioctl_ais kprobe attaches but can never fire.)
#   GFX942  — the MI300X nodes are the ones carrying both NVMe and an
#             AIS-capable amdgpu (6.19+).
#
# Some nodes advertise a GFX feature but carry an empty /opt/rocm stub, and CMake
# then reports "ROCm HIP not found -- skipping examples/" and the capture is
# empty (observed on ctr2-mlse-hpe-alola-s94-13). obs-job.sh detects that and
# aborts with the reason; pass a nodelist as the third argument to pin a
# known-good node if it recurs.

set -euo pipefail

# shellcheck source=lib.sh
source "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/lib.sh"

RUN_ID="${1:-obs-$(date +%Y%m%d-%H%M%S)}"
CONSTRAINT="${2:-GFX942&MARKHAM&NVME}"
NODELIST="${3-}"
RESULTS_DIR="$REPO_ROOT/results/$RUN_ID"
mkdir -p "$RESULTS_DIR"

args=(
    --partition="${HSA_SNOOP_OBS_PARTITION:-defq}"
    --gres=gpu:1
    --time=45
    --job-name="hsasnoop-obs"
    --constraint="$CONSTRAINT"
)
[[ -n $NODELIST ]] && args+=(--nodelist="$NODELIST")
# Known-bad nodes: they advertise a ROCM feature but carry an incomplete
# /opt/rocm (hipcc present, lib/cmake/hip absent), so find_package(hip) fails
# and the capture comes back with no workload binaries. obs-job.sh now detects
# this in its preflight and aborts in seconds, but there is no point landing
# there at all. Override with HSA_SNOOP_OBS_EXCLUDE.
EXCLUDE="${HSA_SNOOP_OBS_EXCLUDE-ctr-cx63-mi300x-12,ctr2-mlse-hpe-alola-s94-13}"
[[ -n $EXCLUDE ]] && args+=(--exclude="$EXCLUDE")

jobid=$(sbatch --parsable \
    "${args[@]}" \
    --output="$RESULTS_DIR/slurm-obs-%j.out" \
    --export="ALL,RESULTS_DIR=$RESULTS_DIR,HSA_SNOOP_REPO=$REPO_ROOT" \
    "$REPO_ROOT/scripts/slurm/obs-job.sh")

log "submitted job $jobid (partition=${HSA_SNOOP_OBS_PARTITION:-defq} constraint=$CONSTRAINT nodelist=${NODELIST:-any})"
log "results  : $RESULTS_DIR"
log "watch    : squeue -j $jobid"
log "then run : scripts/observability/stack.sh up $RESULTS_DIR"
echo "$RESULTS_DIR"
