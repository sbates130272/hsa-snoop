#!/usr/bin/env bash
# Submit the hsa-snoop multi-GFX validation matrix to Slurm.
#
#   scripts/slurm/submit-all.sh [run-id]
#
# Re-running with an existing run-id is idempotent: any target whose results
# directory already contains DONE is skipped, so this can be used to fill in
# gaps after nodes free up.

set -euo pipefail

# shellcheck source=lib.sh
source "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/lib.sh"

RUN_ID="${1:-$(date +%Y%m%d-%H%M%S)}"
RESULTS_DIR="$REPO_ROOT/results/$RUN_ID"
mkdir -p "$RESULTS_DIR"
export RESULTS_DIR

# Every job is pinned to the MARKHAM site. AUSTIN nodes (ixt-*, quanta-*,
# smci355-ccs-aus-*) do not mount this /home, so jobs landing there die before
# they can write even their Slurm output file.
#
# gfx906 (MI60) is deliberately absent: every GFX906 node in defq is AUSTIN-side
# and therefore unreachable under that constraint.
#
# target-name | slurm partition | node feature constraint
TARGETS=(
    "gfx908|defq|GFX908&MARKHAM"
    "gfx90a|defq|GFX90A&MARKHAM"
    "gfx942|defq|GFX942&MARKHAM"
    # gfx950 also lives in perfwork and miopen, but this account is not
    # permitted to submit there ("Invalid account or account/partition
    # combination"), so use the defq MI355X nodes.
    "gfx950|defq|GFX950&MARKHAM"
    "gfx1201|defq|GFX1201&MARKHAM"
    "gfx942-storage|storage|"
)

log "run-id=$RUN_ID results=$RESULTS_DIR"
echo "$RUN_ID" >"$RESULTS_DIR/RUN_ID"

for entry in "${TARGETS[@]}"; do
    IFS='|' read -r name partition constraint <<<"$entry"

    # Skip targets that already produced results in this run.
    if compgen -G "$RESULTS_DIR/*-${name%%-*}/DONE" >/dev/null 2>&1 &&
        [[ $name != *-storage ]]; then
        log "skip $name — already DONE in this run"
        continue
    fi

    args=(
        --partition="$partition"
        --gres=gpu:1
        --time=45
        --job-name="hsasnoop-$name"
        --output="$RESULTS_DIR/slurm-$name-%j.out"
        --export="ALL,RESULTS_DIR=$RESULTS_DIR,HSA_SNOOP_REPO=$REPO_ROOT"
    )
    [[ -n $constraint ]] && args+=(--constraint="$constraint")

    if jobid=$(sbatch --parsable "${args[@]}" "$REPO_ROOT/scripts/slurm/node-job.sh" 2>&1); then
        log "submitted $name -> job $jobid (partition=$partition constraint=${constraint:-none})"
        echo "$name $jobid $partition ${constraint:-none}" >>"$RESULTS_DIR/submitted.txt"
    else
        log "FAILED to submit $name: $jobid"
        echo "$name SUBMIT_FAILED $partition ${constraint:-none} :: $jobid" \
            >>"$RESULTS_DIR/submitted.txt"
    fi
done

log "all submissions attempted; watch with: squeue -u \$USER"
log "aggregate with: scripts/slurm/report.py $RESULTS_DIR"
