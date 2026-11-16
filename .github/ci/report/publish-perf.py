#!/usr/bin/env python3
"""
Append a performance history entry, refresh the shields badges and
write a snapshot of every exported metric.

Reads Prometheus text metrics from --prometheus-file, extracts the
series defined in METRICS, appends a JSONL record to
<out-dir>/history.jsonl, and writes one shields.io badge JSON per series
to <out-dir>/badge-<key>.json. With --snapshot-output it also writes
every metric, grouped by family, as JSON.

These are data files only. The pages that present them are part of the
MkDocs site (docs/perf/, docs/prom/) and fetch them at runtime; the
metric labels there mirror METRICS below (docs/javascripts/perf.js).

Usage, against a checkout of the gh-pages branch:
  python3 .github/ci/report/publish-perf.py \
    --prometheus-file /tmp/hsa-snoop-artifacts/prometheus.txt \
    --out-dir gh-pages/perf \
    --snapshot-output gh-pages/prom/snapshot.json \
    [--runner github-hosted] [--sha <git-sha>] [--run-id <github-run-id>]
"""

import argparse
import datetime
import json
import os
import re
from pathlib import Path

# Metrics to extract from the Prometheus exposition text.
# key       : identifier used in history JSON and badge filename
# metric    : exact Prometheus metric name (ignoring labels)
# label     : optional {key="value"} filter; None = sum all series
# unit      : display unit for badges
# decimals  : decimal places in the badge value
METRICS = [
    {
        "key": "kernel_launches",
        "badge_label": "Kernel Dispatches",
        "metric": "hsa_kernel_launches_total",
        "label": None,
        "unit": "dispatches",
        "decimals": 0,
        "description": "Total HSA kernel dispatches observed",
    },
    {
        "key": "sdma_copies",
        "badge_label": "SDMA Copies",
        "metric": "hsa_sdma_copies_total",
        "label": None,
        "unit": "copies",
        "decimals": 0,
        "description": "Total SDMA copy operations observed",
    },
    {
        "key": "ais_rx_bytes",
        "badge_label": "AIS RX Bytes",
        "metric": "ais_rx_bytes_total",
        "label": None,
        "unit": "bytes",
        "decimals": 0,
        "description": "AIS read bytes total",
    },
    {
        "key": "ais_tx_bytes",
        "badge_label": "AIS TX Bytes",
        "metric": "ais_tx_bytes_total",
        "label": None,
        "unit": "bytes",
        "decimals": 0,
        "description": "AIS write bytes total",
    },
]

# Colour thresholds for shields.io badges (green / yellow / red).
# We always use blue for counter metrics (no meaningful threshold).
BADGE_COLOR = "blue"
NO_DATA_BADGE_COLOR = "lightgrey"


def parse_prometheus(text: str) -> dict[str, float]:
    """Return {metric_name_with_labels: value} from Prometheus text format."""
    values: dict[str, float] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        # metric_name{labels} value [timestamp]
        m = re.match(r'^([a-zA-Z_:][a-zA-Z0-9_:]*(?:\{[^}]*\})?)\s+([-+]?[0-9.eE+\-]+|NaN|[+-]?Inf)\s*', line)
        if m:
            values[m.group(1)] = float(m.group(2)) if m.group(2) not in ("NaN", "+Inf", "-Inf") else 0.0
    return values


def extract_metric(prom_values: dict[str, float], metric: str, label_filter: str | None) -> float:
    """Sum all series matching metric name (and optional label substring)."""
    total = 0.0
    for key, val in prom_values.items():
        base = key.split("{", 1)[0]
        if base != metric:
            continue
        if label_filter is not None and label_filter not in key:
            continue
        total += val
    return total


def badge_json(label: str, value: float, unit: str, decimals: int) -> dict:
    if decimals == 0:
        msg = f"{int(value):,} {unit}"
    else:
        msg = f"{value:.{decimals}f} {unit}"
    return {
        "schemaVersion": 1,
        "label": label,
        "message": msg,
        "color": BADGE_COLOR,
    }


def no_data_badge_json(label: str) -> dict:
    return {
        "schemaVersion": 1,
        "label": label,
        "message": "no data",
        "color": NO_DATA_BADGE_COLOR,
    }


def write_badges(history: list[dict], out_dir: Path) -> None:
    latest_metrics = history[-1].get("metrics", {}) if history else {}
    for m in METRICS:
        badge_path = out_dir / f"badge-{m['key']}.json"
        val = latest_metrics.get(m["key"])
        bl = m.get("badge_label", m["key"])
        if val is None:
            badge_path.write_text(json.dumps(no_data_badge_json(bl)))
            continue
        badge_path.write_text(json.dumps(
            badge_json(bl, val, m["unit"], m["decimals"])))


_SUFFIXES = ("_total", "_created", "_seconds_total", "_bytes_total",
             "_count", "_sum", "_bucket")


def _family(name: str) -> str:
    for sfx in _SUFFIXES:
        if name.endswith(sfx):
            return name[: -len(sfx)]
    return name


def write_snapshot(prom_text: str, out_path: Path, sha: str, run_id: str, runner: str) -> None:
    """Write every metric in prom_text, grouped by family, as JSON."""
    families: dict[str, list[dict]] = {}
    for raw_key, val in sorted(parse_prometheus(prom_text).items()):
        family = _family(raw_key.split("{", 1)[0])
        families.setdefault(family, []).append({"name": raw_key, "value": val})

    # HELP/TYPE lines may carry either the full metric name or the family.
    help_map: dict[str, str] = {}
    type_map: dict[str, str] = {}
    for line in prom_text.splitlines():
        for prefix, dest in (("# HELP ", help_map), ("# TYPE ", type_map)):
            if line.startswith(prefix):
                parts = line[len(prefix):].split(" ", 1)
                if len(parts) == 2:
                    dest[_family(parts[0])] = parts[1]

    snapshot = {
        "generated": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
        "sha": sha,
        "run_id": run_id,
        "runner": runner,
        "families": [
            {
                "name": family,
                "type": type_map.get(family, ""),
                "help": help_map.get(family, ""),
                "series": series,
            }
            for family, series in sorted(families.items())
        ],
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(snapshot) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prometheus-file", required=True,
                    help="Path to Prometheus exposition text from hsa-snoop")
    ap.add_argument("--out-dir", required=True,
                    help="Directory holding history.jsonl and the badges (perf/ on gh-pages)")
    ap.add_argument("--runner", default="github-hosted", help="Runner class label")
    ap.add_argument("--sha", default=os.environ.get("GITHUB_SHA", ""))
    ap.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    ap.add_argument("--snapshot-output", metavar="PATH",
                    help="Also write a JSON snapshot of every metric to this path")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    history_path = out_dir / "history.jsonl"
    out_dir.mkdir(parents=True, exist_ok=True)

    history: list[dict] = []
    if history_path.exists():
        for line in history_path.read_text().splitlines():
            line = line.strip()
            if line:
                try:
                    history.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    prom_text = Path(args.prometheus_file).read_text()
    prom_values = parse_prometheus(prom_text)

    record: dict = {
        "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z"),
        "meta": {
            "sha": args.sha,
            "run_id": args.run_id,
            "runner": args.runner,
        },
        "metrics": {},
    }
    for m in METRICS:
        record["metrics"][m["key"]] = extract_metric(prom_values, m["metric"], m.get("label"))

    history.append(record)
    with history_path.open("a") as fh:
        fh.write(json.dumps(record) + "\n")
    print(f"appended record to {history_path} ({len(history)} total)")

    write_badges(history, out_dir)
    print(f"wrote badges to {out_dir}")

    if args.snapshot_output:
        write_snapshot(prom_text, Path(args.snapshot_output), args.sha, args.run_id, args.runner)
        print(f"wrote snapshot to {args.snapshot_output}")


if __name__ == "__main__":
    main()
