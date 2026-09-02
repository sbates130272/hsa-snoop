#!/usr/bin/env bash
# Bring up an offline Grafana + Prometheus + Loki stack over a capture produced
# by scripts/slurm/obs-job.sh, so the dashboard can be developed on real
# hsa-snoop data without holding a GPU allocation.
#
#   scripts/observability/stack.sh up <results-dir|capture-dir>
#   scripts/observability/stack.sh reload           # re-provision grafana/*.json only
#   scripts/observability/stack.sh down
#   scripts/observability/stack.sh status
#   scripts/observability/stack.sh logs [service]
#
# Prometheus and Loki always bind 127.0.0.1 (Grafana reaches them over the
# compose network; nothing off-host needs them). Grafana binds 127.0.0.1 too by
# default -- set HSA_SNOOP_BIND=0.0.0.0 to reach it from another machine.
# Grafana runs with anonymous admin and no login in every case. Override ports
# with HSA_SNOOP_PORT_GRAFANA / _PROM / _LOKI.

set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
SCRATCH="${HSA_SNOOP_OBS_SCRATCH:-$REPO_ROOT/.obs-scratch}"

# compose interpolates the bind-mount sources for EVERY subcommand, including
# `down` and `ps`. Unset variables there are a hard error, so give them
# placeholder values that `up` overwrites; otherwise teardown fails on a stack
# that is plainly running.
export HSA_SNOOP_CAPTURE="${HSA_SNOOP_CAPTURE:-$SCRATCH}"
export HSA_SNOOP_TSDB="${HSA_SNOOP_TSDB:-$SCRATCH/tsdb}"
export HSA_SNOOP_DASHBOARDS="${HSA_SNOOP_DASHBOARDS:-$SCRATCH/dashboards}"
export HSA_SNOOP_CAPTURE_ID="${HSA_SNOOP_CAPTURE_ID:-unknown}"
export HSA_SNOOP_BIND="${HSA_SNOOP_BIND:-127.0.0.1}"
mkdir -p "$SCRATCH/tsdb" "$SCRATCH/dashboards"

log() { printf '[%s] %s\n' "$(date -Is)" "$*"; }
die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

# Accept either the capture directory itself or the run's results directory, and
# resolve the latter down to the single <node>-<arch>/obs inside it. Typing the
# full node-and-arch path is the kind of thing that gets wrong twice out of
# three times when the node changes between runs.
resolve_capture() {
    local d="$1"
    [[ -d $d ]] || die "no such directory: $d"
    # Absolute: compose treats a relative volume source as a NAMED VOLUME, so a
    # relative capture path fails with "refers to undefined volume" rather than
    # bind-mounting anything.
    d="$(readlink -f "$d")"
    if [[ -f $d/params.txt && -d $d/tsdb ]]; then
        echo "$d"
        return
    fi
    local -a found
    mapfile -t found < <(find "$d" -maxdepth 2 -type d -name obs | sort)
    ((${#found[@]} == 1)) ||
        die "expected exactly one <node>-<arch>/obs under $d, found ${#found[@]}"
    echo "${found[0]}"
}

# Prometheus runs as root in this stack (see docker-compose.yml), so the WAL and
# chunk files it leaves in the scratch directory are root-owned and the invoking
# user cannot delete them. Do the removal from a throwaway root container.
purge_scratch() {
    local target="$1"
    [[ -e $target ]] || return 0
    rm -rf "$target" 2>/dev/null && return 0
    docker run --rm -v "$(dirname "$target")":/scratch \
        "${HSA_SNOOP_BASE_IMAGE:-ubuntu:24.04}" \
        rm -rf "/scratch/$(basename "$target")" >/dev/null 2>&1 ||
        die "cannot remove $target (is the stack still running? try: stack.sh down)"
}

# Prometheus refuses to open a data directory it cannot write (it needs a WAL
# and a lock file), and it will happily compact and expire the blocks it is
# given. Both are unacceptable against an artefact under results/, so replay
# always runs against a copy.
stage_tsdb() {
    local capture="$1" dest="$2"
    local snap_root="$capture/tsdb/snapshots"
    local src=""

    if [[ -d $snap_root ]]; then
        src="$(find "$snap_root" -mindepth 1 -maxdepth 1 -type d | sort | tail -1)"
    fi
    if [[ -z $src ]]; then
        # No snapshot: fall back to the raw tsdb directory, which for a short
        # run is a WAL with no compacted blocks. Prometheus replays that on
        # startup, so the data is all there, but say so — a missing snapshot
        # means the admin API call in obs-job.sh failed and that is worth
        # knowing rather than silently working.
        log "WARNING: no TSDB snapshot in $snap_root; replaying the raw WAL instead"
        src="$capture/tsdb"
    else
        log "using snapshot $(basename "$src")"
    fi

    purge_scratch "$dest"
    mkdir -p "$dest"
    # Exclude the snapshots subtree when copying the raw directory, or the
    # fallback path duplicates every block and Prometheus loads each series
    # twice.
    tar -C "$src" --exclude=snapshots --exclude=lock --exclude=queries.active \
        -cf - . | tar -C "$dest" -xf -
    chmod -R u+w "$dest"
}

# The bundled dashboard is a grafana.com "export for sharing externally" file:
# its datasources are the placeholder ${DS_PROMETHEUS} fed by an __inputs block
# that only the import wizard understands. File provisioning does not run that
# wizard, so provisioning it verbatim yields a dashboard where every panel
# reports "Datasource ${DS_PROMETHEUS} was not found". Resolve it on the way in.
stage_dashboards() {
    local dest="$1"
    # Overwrite in place; do NOT remove and recreate the directory. It is
    # bind-mounted into a running Grafana, and on NFS a fresh inode at the same
    # path turns the container's mount into a stale file handle -- provisioning
    # then fails with "failed to walk provisioned dashboards" until the whole
    # stack is restarted, which defeats the point of `reload`.
    mkdir -p "$dest"
    rm -f "$dest"/*.json
    python3 - "$REPO_ROOT/grafana" "$dest" <<'PY'
import json, pathlib, sys

src, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
PROM, LOKI = "hsa-snoop-prom", "hsa-snoop-loki"

def fix(node):
    if isinstance(node, dict):
        # A datasource ref is {"type": ..., "uid": ...}; rewrite the uid only
        # when it is an unresolved template, so any dashboard that already
        # pins a real uid is left alone.
        if "uid" in node and isinstance(node.get("uid"), str) \
                and node["uid"].startswith("${DS_"):
            node["uid"] = LOKI if node.get("type") == "loki" else PROM
        return {k: fix(v) for k, v in node.items() if k not in ("__inputs", "__requires")}
    if isinstance(node, list):
        return [fix(v) for v in node]
    return node

for f in sorted(src.glob("*.json")):
    d = fix(json.loads(f.read_text()))
    # Provisioned dashboards must not carry an id from another Grafana.
    d["id"] = None
    (dest / f.name).write_text(json.dumps(d, indent=2))
    print(f"staged {f.name} (uid={d.get('uid')}, panels={len(d.get('panels', []))})")
PY
}

compose() {
    docker compose --project-directory "$HERE" -f "$HERE/docker-compose.yml" "$@"
}

# This runs on a SHARED login node where 3000 and 3100 are routinely already
# taken by somebody else's Grafana and Loki. Without this the stack half-starts
# and then dies on "bind: address already in use" with two containers left
# running, which is a confusing failure for something that is purely local.
pick_port() {
    local want="$1" p
    for p in $(seq "$want" $((want + 40))); do
        if ! (ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN); then
            echo "$p"
            return
        fi
    done
    die "no free port in [$want, $((want + 40))]"
}

wait_http() {
    local name="$1" url="$2" tries="${3:-90}"
    for _ in $(seq 1 "$tries"); do
        if curl -sf --max-time 3 "$url" >/dev/null 2>&1; then
            log "  $name ready"
            return 0
        fi
        sleep 1
    done
    log "  WARNING: $name never became ready at $url"
    return 1
}

cmd_up() {
    local target="${1:-}"
    [[ -n $target ]] || die "usage: stack.sh up <results-dir|capture-dir>"
    local capture
    capture="$(resolve_capture "$target")"
    [[ -f $capture/DONE ]] || log "WARNING: $capture has no DONE marker; capture may be incomplete"
    log "capture: $capture"

    local capture_id
    capture_id="$(basename "$(dirname "$capture")")"

    # Teardown FIRST, before staging. Prometheus holds the scratch TSDB open; on
    # NFS that turns every delete into a .nfsXXXX silly-rename and the restage
    # fails with "Device or resource busy".
    compose down -v --remove-orphans >/dev/null 2>&1 || true

    stage_tsdb "$capture" "$SCRATCH/tsdb"
    stage_dashboards "$SCRATCH/dashboards"

    # Exported, not set per-command: `compose down` needs them too. Without
    # them compose cannot interpolate the bind-mount sources, errors out, and —
    # because the teardown below is best-effort — leaves the old containers
    # running. They then survive `up` untouched (a changed bind-mounted config
    # file is not part of a container's spec, so compose sees nothing to
    # recreate) and the stack silently keeps serving the previous config.
    export HSA_SNOOP_CAPTURE="$capture"
    export HSA_SNOOP_TSDB="$SCRATCH/tsdb"
    export HSA_SNOOP_DASHBOARDS="$SCRATCH/dashboards"
    export HSA_SNOOP_CAPTURE_ID="$capture_id"

    local gport pport lport
    gport="${HSA_SNOOP_PORT_GRAFANA:-$(pick_port 3000)}"
    pport="${HSA_SNOOP_PORT_PROM:-$(pick_port 9490)}"
    lport="${HSA_SNOOP_PORT_LOKI:-$(pick_port 3100)}"
    export HSA_SNOOP_PORT_GRAFANA="$gport"
    export HSA_SNOOP_PORT_PROM="$pport"
    export HSA_SNOOP_PORT_LOKI="$lport"

    # --force-recreate for the same reason: only the mounted config files change
    # between iterations, and compose would otherwise reuse the old containers.
    compose up -d --force-recreate

    log "waiting for services"
    wait_http prometheus "http://127.0.0.1:$pport/-/ready" || true
    wait_http loki "http://127.0.0.1:$lport/ready" 120 || true
    wait_http grafana "http://127.0.0.1:$gport/api/health" || true

    # Report the actual time span of the replayed data. Without this the first
    # thing anyone sees is an empty dashboard, because the default time range is
    # now-30m and the capture is from whenever the Slurm job happened to run.
    local span
    span="$(curl -sG --max-time 10 "http://127.0.0.1:$pport/api/v1/query" \
        --data-urlencode 'query=hsa_snoop_up' 2>/dev/null || true)"
    log ""
    local host="127.0.0.1"
    if [[ $HSA_SNOOP_BIND != "127.0.0.1" && $HSA_SNOOP_BIND != "localhost" ]]; then
        # The address someone else has to type, not the wildcard we bound to.
        host="$(ip -4 -o addr show scope global 2>/dev/null |
            awk '$2 !~ /^(docker|veth|br-|flannel|cni|usb)/ {split($4,a,"/"); print a[1]; exit}')"
        host="${host:-$(hostname -f)}"
    fi
    log "Grafana    http://$host:$gport  (dashboard: hsa-snoop folder)"
    log "Prometheus http://127.0.0.1:$pport  (loopback only)"
    log "Loki       http://127.0.0.1:$lport  (loopback only)"
    log ""
    if [[ -f $capture/params.txt ]]; then
        log "capture window (set Grafana's time range to cover this):"
        sed 's/^/    /' "$capture/params.txt"
    fi
    [[ -n $span ]] && log "prometheus sees hsa_snoop_up: $(echo "$span" | head -c 200)"
}

# Restage grafana/*.json into the running stack without touching Prometheus or
# re-ingesting Loki. This is the inner loop of dashboard work: a full `up` costs
# a Loki re-ingest and a TSDB recopy for a change to one JSON file.
cmd_reload() {
    docker inspect hsasnoop-grafana >/dev/null 2>&1 ||
        die "stack is not running; use: stack.sh up <results-dir>"
    stage_dashboards "$SCRATCH/dashboards"
    log "restaged; grafana re-reads provisioned dashboards within 10s"
}

cmd_down() {
    compose down -v --remove-orphans
    purge_scratch "$SCRATCH"
    log "stack down, scratch removed"
}

cmd_status() {
    compose ps
}

cmd_logs() {
    compose logs --tail=100 "$@"
}

case "${1:-}" in
    up)
        shift
        cmd_up "$@"
        ;;
    reload) cmd_reload ;;
    down) cmd_down ;;
    status) cmd_status ;;
    logs)
        shift
        cmd_logs "$@"
        ;;
    *)
        die "usage: stack.sh {up <results-dir>|reload|down|status|logs [service]}"
        ;;
esac
