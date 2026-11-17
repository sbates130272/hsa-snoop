#!/usr/bin/env python3
"""
Convert prom/snapshot.json to VictoriaMetrics import format.

Reads the JSON snapshot written by publish-perf.py and emits Prometheus
exposition text with UNIX-millisecond timestamps so VictoriaMetrics can
ingest the full metric set from the last hardware test run:

    python3 .github/ci/report/gen-prom-snapshot.py prom/snapshot.json | \
        curl -X POST http://localhost:8428/api/v1/import/prometheus \
             -H 'Content-Type: text/plain' --data-binary @-
"""

import json
import sys
from datetime import datetime, timezone


def iso_to_unix_ms(ts: str) -> int:
    dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
    return int(dt.timestamp() * 1000)


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else "/dev/stdin"
    with open(src) as f:
        snap = json.load(f)

    ts_ms = iso_to_unix_ms(snap["generated"])
    for family in snap.get("families", []):
        for series in family.get("series", []):
            print(f'{series["name"]} {series["value"]} {ts_ms}')


if __name__ == "__main__":
    main()
