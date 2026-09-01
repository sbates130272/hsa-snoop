#!/usr/bin/env bash
# Ad-hoc PromQL / LogQL against the running replay stack.
#
#   scripts/observability/q.sh prom '<promql>' [start] [end] [step]
#   scripts/observability/q.sh loki '<logql>'  [start] [end] [limit]
#   scripts/observability/q.sh labels
#
# start/end are unix seconds; omit them for an instant query. stack.sh prints
# the capture window on `up`.
#
# Talks to the published 127.0.0.1 ports, discovered with `docker port` so that
# stack.sh's automatic port selection on a busy login node does not have to be
# repeated here.

set -euo pipefail

port_of() {
    local ctr="$1" cport="$2" mapped
    mapped="$(docker port "$ctr" "$cport" 2>/dev/null | head -1)" ||
        { echo "stack not running: no container $ctr" >&2; exit 1; }
    [[ -n $mapped ]] || { echo "container $ctr has no published $cport" >&2; exit 1; }
    echo "${mapped##*:}"
}

case "${1:-}" in
    prom)
        q="${2:?promql required}"
        start="${3:-}" end="${4:-}" step="${5:-15}"
        base="http://127.0.0.1:$(port_of hsasnoop-prometheus 9090)"
        if [[ -n $start ]]; then
            curl -sG --max-time 60 "$base/api/v1/query_range" \
                --data-urlencode "query=$q" \
                --data-urlencode "start=$start" \
                --data-urlencode "end=$end" \
                --data-urlencode "step=$step"
        else
            curl -sG --max-time 60 "$base/api/v1/query" --data-urlencode "query=$q"
        fi
        ;;
    loki)
        q="${2:?logql required}"
        start="${3:-}" end="${4:-}" limit="${5:-100}"
        base="http://127.0.0.1:$(port_of hsasnoop-loki 3100)"
        args=(--data-urlencode "query=$q" --data-urlencode "limit=$limit")
        # Loki wants nanoseconds. Seconds are accepted as a timestamp near the
        # epoch and return an empty result, which reads exactly like "nothing
        # was ingested" and sends you debugging the wrong component.
        [[ -n $start ]] && args+=(--data-urlencode "start=${start}000000000")
        [[ -n $end ]] && args+=(--data-urlencode "end=${end}000000000")
        curl -sG --max-time 60 "$base/loki/api/v1/query_range" "${args[@]}"
        ;;
    labels)
        curl -sG --max-time 30 \
            "http://127.0.0.1:$(port_of hsasnoop-prometheus 9090)/api/v1/label/__name__/values"
        echo
        curl -sG --max-time 30 \
            "http://127.0.0.1:$(port_of hsasnoop-loki 3100)/loki/api/v1/labels"
        ;;
    *)
        echo "usage: q.sh {prom <promql>|loki <logql>|labels} [start] [end] [step|limit]" >&2
        exit 1
        ;;
esac
