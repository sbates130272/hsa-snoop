#!/usr/bin/env python3
"""
Convert perf/history.jsonl records to VictoriaMetrics import format.

Reads JSONL from stdin (or the file given as the first argument) and writes
Prometheus exposition text with UNIX-millisecond timestamps to stdout so that
VictoriaMetrics can ingest the full performance history:

    python3 .github/ci/report/gen-prom-history.py perf/history.jsonl | \
        curl -X POST http://localhost:8428/api/v1/import/prometheus \
             -H 'Content-Type: text/plain' --data-binary @-

Each history record produces one line per metric in the form:
    <metric_name>{host="<runner>",sha="<sha>"} <value> <unix_ms>
"""

import json
import sys
from datetime import datetime, timezone

METRIC_MAP = {
    "kernel_launches": "hsa_kernel_launches_total",
    "sdma_copies":     "hsa_sdma_copies_total",
    "ais_rx_bytes":    "ais_rx_bytes_total",
    "ais_tx_bytes":    "ais_tx_bytes_total",
}


def iso_to_unix_ms(ts: str) -> int:
    dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    return int(dt.timestamp() * 1000)


def main() -> None:
    src = open(sys.argv[1]) if len(sys.argv) > 1 else sys.stdin
    for raw in src:
        raw = raw.strip()
        if not raw:
            continue
        try:
            rec = json.loads(raw)
        except json.JSONDecodeError:
            continue
        ts_ms = iso_to_unix_ms(rec["timestamp"])
        meta = rec.get("meta", {})
        sha = meta.get("sha", "unknown")[:12]
        runner = meta.get("runner", "unknown")
        metrics = rec.get("metrics", {})
        for key, prom_name in METRIC_MAP.items():
            val = metrics.get(key)
            if val is None:
                continue
            labels = f'{{host="{runner}",sha="{sha}"}}'
            print(f"{prom_name}{labels} {val:.0f} {ts_ms}")


if __name__ == "__main__":
    main()
