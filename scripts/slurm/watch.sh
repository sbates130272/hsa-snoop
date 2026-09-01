#!/usr/bin/env bash
# Unattended retry loop for the hsa-snoop validation matrix.
#
#   scripts/slurm/watch.sh <results-dir> [max-rounds] [sleep-seconds]
#
# Some cluster nodes advertise a ROCM* feature but carry only an empty
# /opt/rocm stub, so an arch can land on an unusable node through no fault of
# the code. Each round this script finds every target arch that still has no
# successful build, excludes the nodes already tried for it, and resubmits.
# It also refreshes the aggregated report after every round.

set -uo pipefail

# shellcheck source=lib.sh
source "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/lib.sh"
set +e

RESULTS_DIR="${1:?usage: watch.sh <results-dir> [max-rounds] [sleep-seconds]}"
MAX_ROUNDS="${2:-24}"
SLEEP_SECS="${3:-300}"
export RESULTS_DIR

# arch | partition | constraint  (storage is handled by its own long-pending job)
TARGETS=(
    "gfx908|defq|GFX908&MARKHAM"
    "gfx90a|defq|GFX90A&MARKHAM"
    "gfx942|defq|GFX942&MARKHAM"
    "gfx950|defq|GFX950&MARKHAM"
    "gfx1201|defq|GFX1201&MARKHAM"
)

# Has this arch produced a directory with a successful build?
arch_is_good() {
    local arch="$1" d
    for d in "$RESULTS_DIR"/*-"$arch"/; do
        [[ -f $d/build.rc ]] && [[ $(cat "$d/build.rc") == 0 ]] && return 0
    done
    return 1
}

# Nodes already attempted for this arch, as a comma-separated --exclude list.
tried_nodes() {
    local arch="$1" d base out=()
    for d in "$RESULTS_DIR"/*-"$arch"/; do
        [[ -d $d ]] || continue
        base="$(basename "$d")"
        out+=("${base%-"$arch"}")
    done
    # Nodes whose arch was never detected are also dead ends.
    for d in "$RESULTS_DIR"/*-unknown/; do
        [[ -d $d ]] || continue
        base="$(basename "$d")"
        out+=("${base%-unknown}")
    done
    (IFS=,; echo "${out[*]}")
}

for ((round = 1; round <= MAX_ROUNDS; round++)); do
    pending=0
    for entry in "${TARGETS[@]}"; do
        IFS='|' read -r arch partition constraint <<<"$entry"

        if arch_is_good "$arch"; then
            continue
        fi
        pending=$((pending + 1))

        # Do not pile up: skip if a job for this arch is already queued/running.
        if squeue -u "$USER" -h -o "%j" | grep -qx "hsasnoop-$arch"; then
            log "round $round: $arch already in queue, waiting"
            continue
        fi

        exclude="$(tried_nodes "$arch")"
        args=(
            --partition="$partition"
            --gres=gpu:1
            --time=45
            --constraint="$constraint"
            --job-name="hsasnoop-$arch"
            --output="$RESULTS_DIR/slurm-$arch-%j.out"
            --export="ALL,RESULTS_DIR=$RESULTS_DIR,HSA_SNOOP_REPO=$REPO_ROOT"
        )
        [[ -n $exclude ]] && args+=(--exclude="$exclude")

        if jobid=$(sbatch --parsable "${args[@]}" \
            "$REPO_ROOT/scripts/slurm/node-job.sh" 2>&1); then
            log "round $round: resubmitted $arch -> job $jobid (excluding ${exclude:-none})"
        else
            log "round $round: $arch submit failed: $jobid"
        fi
    done

    python3 "$REPO_ROOT/scripts/slurm/report.py" "$RESULTS_DIR" \
        --markdown "$RESULTS_DIR/report.md" >/dev/null 2>&1

    if [[ $pending -eq 0 ]] && ! squeue -u "$USER" -h -o "%j" | grep -q hsasnoop; then
        log "all targets have a successful build and no jobs remain; stopping"
        break
    fi
    log "round $round complete; $pending target(s) outstanding; sleeping ${SLEEP_SECS}s"
    sleep "$SLEEP_SECS"
done

log "watch loop finished"
