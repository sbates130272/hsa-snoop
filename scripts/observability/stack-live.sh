#!/usr/bin/env bash
# Live hsa-snoop observability stack.
#
# Bring up Grafana + Prometheus + Loki + Alloy against a running hsa-snoop
# instance. Unlike stack.sh (replay), this tails live files and scrapes a live
# Prometheus endpoint — no capture directory required.
#
# Usage:
#   stack-live.sh up [--log-dir <dir>] [--prom-port <n>]
#   stack-live.sh reload      # re-stage dashboards only
#   stack-live.sh down
#   stack-live.sh status
#   stack-live.sh logs [service]
#
# hsa-snoop must be running with:
#   hsa-snoop --prometheus [--prometheus-port <n>] \
#             --log-dispatches --dispatch-log <log-dir>/dispatches.ndjson \
#             [workload args] 2><log-dir>/hsa-snoop.log
#
# Grafana binds 127.0.0.1:3000 by default.
# Set HSA_SNOOP_BIND=0.0.0.0 to expose it to other machines (e.g. over SSH tunnel).
# Override ports with HSA_SNOOP_PORT_GRAFANA / _PROM / _LOKI.

set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"
SCRATCH="${HSA_SNOOP_OBS_SCRATCH:-$REPO_ROOT/.obs-live-scratch}"

export HSA_SNOOP_BIND="${HSA_SNOOP_BIND:-127.0.0.1}"
export HSA_SNOOP_DASHBOARDS="${HSA_SNOOP_DASHBOARDS:-$SCRATCH/dashboards}"
export HSA_SNOOP_LOG_DIR="${HSA_SNOOP_LOG_DIR:-/tmp}"
export HSA_SNOOP_PROM_PORT="${HSA_SNOOP_PROM_PORT:-9488}"
mkdir -p "$SCRATCH/dashboards"

log() { printf '[%s] %s\n' "$(date -Is)" "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

stage_dashboards() {
    local dest="$1"
    mkdir -p "$dest"
    rm -f "$dest"/*.json
    python3 - "$REPO_ROOT/grafana" "$dest" <<'PY'
import json, pathlib, sys

src, dest = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
PROM, LOKI = "hsa-snoop-prom", "hsa-snoop-loki"

def fix(node):
    if isinstance(node, dict):
        if "uid" in node and isinstance(node.get("uid"), str) \
                and node["uid"].startswith("${DS_"):
            node["uid"] = LOKI if node.get("type") == "loki" else PROM
        return {k: fix(v) for k, v in node.items() if k not in ("__inputs", "__requires")}
    if isinstance(node, list):
        return [fix(v) for v in node]
    return node

for f in sorted(src.glob("*.json")):
    d = fix(json.loads(f.read_text()))
    d["id"] = None
    (dest / f.name).write_text(json.dumps(d, indent=2))
    print(f"staged {f.name} (uid={d.get('uid')}, panels={len(d.get('panels', []))})")
PY
}

pick_port() {
    local want="$1" p
    for p in $(seq "$want" $((want + 40))); do
        if ! (ss -ltn "sport = :$p" 2>/dev/null | grep -q LISTEN); then
            echo "$p"; return
        fi
    done
    die "no free port in [$want, $((want + 40))]"
}

wait_http() {
    local name="$1" url="$2" tries="${3:-90}"
    for _ in $(seq 1 "$tries"); do
        if curl -sf --max-time 3 "$url" >/dev/null 2>&1; then
            log "  $name ready"; return 0
        fi
        sleep 1
    done
    log "  WARNING: $name never became ready at $url"; return 1
}

compose() {
    docker compose --project-directory "$HERE" -f "$HERE/docker-compose-live.yml" "$@"
}

cmd_up() {
    local log_dir="$HSA_SNOOP_LOG_DIR"
    local prom_port="$HSA_SNOOP_PROM_PORT"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --log-dir)   log_dir="$2";  shift 2 ;;
            --prom-port) prom_port="$2"; shift 2 ;;
            *) die "unknown option: $1" ;;
        esac
    done

    [[ -d $log_dir ]] || die "log dir does not exist: $log_dir"

    export HSA_SNOOP_LOG_DIR="$log_dir"
    export HSA_SNOOP_PROM_PORT="$prom_port"

    log "log dir:   $log_dir"
    log "prom port: $prom_port"

    compose down -v --remove-orphans >/dev/null 2>&1 || true
    stage_dashboards "$SCRATCH/dashboards"
    export HSA_SNOOP_DASHBOARDS="$SCRATCH/dashboards"

    # Prometheus config does not expand shell variables, so write a resolved copy.
    mkdir -p "$SCRATCH"
    sed "s|\${HSA_SNOOP_PROM_PORT:-9488}|$prom_port|g" \
        "$HERE/prometheus-live.yml" > "$SCRATCH/prometheus-live-resolved.yml"
    export HSA_SNOOP_PROM_CONFIG="$SCRATCH/prometheus-live-resolved.yml"

    local gport pport lport
    gport="${HSA_SNOOP_PORT_GRAFANA:-$(pick_port 3000)}"
    pport="${HSA_SNOOP_PORT_PROM:-$(pick_port 9490)}"
    lport="${HSA_SNOOP_PORT_LOKI:-$(pick_port 3100)}"
    export HSA_SNOOP_PORT_GRAFANA="$gport"
    export HSA_SNOOP_PORT_PROM="$pport"
    export HSA_SNOOP_PORT_LOKI="$lport"

    compose up -d --force-recreate

    log "waiting for services"
    wait_http prometheus "http://127.0.0.1:$pport/-/ready" || true
    wait_http loki       "http://127.0.0.1:$lport/ready" 120 || true
    wait_http grafana    "http://127.0.0.1:$gport/api/health" || true

    local host="127.0.0.1"
    if [[ $HSA_SNOOP_BIND != "127.0.0.1" && $HSA_SNOOP_BIND != "localhost" ]]; then
        host="$(ip -4 -o addr show scope global 2>/dev/null |
            awk '$2 !~ /^(docker|veth|br-)/ {split($4,a,"/"); print a[1]; exit}')"
        host="${host:-$(hostname -f)}"
    fi

    log ""
    log "Grafana    http://$host:$gport  (dashboard: hsa-snoop folder)"
    log "Prometheus http://127.0.0.1:$pport  (loopback only)"
    log "Loki       http://127.0.0.1:$lport  (loopback only)"
    log ""
    log "Expected hsa-snoop log files:"
    log "  $log_dir/dispatches.ndjson"
    log "  $log_dir/hsa-snoop.log"
    [[ -f $log_dir/dispatches.ndjson ]] || log "  WARNING: dispatches.ndjson not found — start hsa-snoop with --log-dispatches"
}

cmd_reload() {
    docker inspect hsasnoop-live-grafana >/dev/null 2>&1 ||
        die "live stack is not running; use: stack-live.sh up"
    stage_dashboards "$SCRATCH/dashboards"
    log "restaged; grafana re-reads provisioned dashboards within 10s"
}

cmd_down() {
    compose down -v --remove-orphans
    rm -rf "$SCRATCH"
    log "live stack down"
}

case "${1:-}" in
    up)     shift; cmd_up "$@" ;;
    reload) cmd_reload ;;
    down)   cmd_down ;;
    status) compose ps ;;
    logs)   shift; compose logs --tail=100 "$@" ;;
    *)      die "usage: stack-live.sh {up [--log-dir <dir>] [--prom-port <n>]|reload|down|status|logs [service]}" ;;
esac
