#!/usr/bin/env python3
"""
Append a performance history entry and refresh shields badges.

Reads Prometheus text metrics from --prometheus-file, extracts the
series defined in METRICS, appends a JSONL record to
docs/perf-history/history.jsonl, and writes one shields.io badge JSON
per series to docs/perf-history/badge-<key>.json.

Usage (from the repo root):
  python3 ci/report/publish-perf.py \
    --prometheus-file /tmp/hsa-snoop-artifacts/prometheus.txt \
    --docs-dir docs \
    --runner github-hosted \
    [--sha <git-sha>] [--run-id <github-run-id>]

When --render-only is passed: regenerate the HTML trend page from the
existing history.jsonl without appending a new record.  Used by the
docs-deploy workflow to rebuild the site from the published history.
"""

import argparse
import datetime
import json
import os
import re
import sys
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
        "metric": "hsa_kernel_launches_total",
        "label": None,
        "unit": "dispatches",
        "decimals": 0,
        "description": "Total HSA kernel dispatches observed",
    },
    {
        "key": "sdma_copies",
        "metric": "hsa_sdma_copies_total",
        "label": None,
        "unit": "copies",
        "decimals": 0,
        "description": "Total SDMA copy operations observed",
    },
    {
        "key": "ais_rx_bytes",
        "metric": "ais_rx_bytes_total",
        "label": None,
        "unit": "bytes",
        "decimals": 0,
        "description": "AIS read bytes total",
    },
    {
        "key": "ais_tx_bytes",
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


def render_html(history: list[dict], out_path: Path) -> None:
    """Write a minimal Chart.js trend page from the history records."""
    keys = [m["key"] for m in METRICS]
    labels_js = json.dumps([r.get("meta", {}).get("sha", r.get("timestamp", ""))[:8] for r in history])
    datasets = []
    colors = ["#0071c5", "#e87a2d", "#43a047", "#e53935", "#8e24aa", "#00838f"]
    for i, m in enumerate(METRICS):
        data = [r.get("metrics", {}).get(m["key"], None) for r in history]
        datasets.append({
            "label": m["key"].replace("_", " "),
            "data": data,
            "borderColor": colors[i % len(colors)],
            "backgroundColor": colors[i % len(colors)] + "33",
            "tension": 0.3,
            "pointRadius": 3,
        })
    datasets_js = json.dumps(datasets)
    html = f"""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>hsa-snoop performance history</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  body {{ font-family: sans-serif; max-width: 900px; margin: 2em auto; padding: 0 1em; }}
  h1 {{ font-size: 1.4em; }}
  .chart-wrap {{ position: relative; height: 320px; margin: 2em 0; }}
</style>
</head>
<body>
<h1>hsa-snoop performance history</h1>
<p>Each point is one CI run on the default branch.  Counters are totals from
a fixed workload duration; higher is more activity observed.</p>
<div class="chart-wrap"><canvas id="perf"></canvas></div>
<script>
const labels = {labels_js};
const datasets = {datasets_js};
new Chart(document.getElementById('perf'), {{
  type: 'line',
  data: {{ labels, datasets }},
  options: {{
    responsive: true,
    maintainAspectRatio: false,
    plugins: {{ legend: {{ position: 'bottom' }} }},
    scales: {{ y: {{ beginAtZero: true }} }},
  }},
}});
</script>
<p><small>Generated by <code>ci/report/publish-perf.py</code>. Source: <a href="https://github.com/sbates130272/hsa-snoop">sbates130272/hsa-snoop</a>.</small></p>
</body>
</html>
"""
    out_path.write_text(html)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prometheus-file", help="Path to Prometheus exposition text from hsa-snoop")
    ap.add_argument("--docs-dir", default="docs", help="Repository docs directory (default: docs)")
    ap.add_argument("--runner", default="github-hosted", help="Runner class label")
    ap.add_argument("--sha", default=os.environ.get("GITHUB_SHA", ""))
    ap.add_argument("--run-id", default=os.environ.get("GITHUB_RUN_ID", ""))
    ap.add_argument("--render-only", action="store_true",
                    help="Regenerate HTML from existing history without appending a new record")
    args = ap.parse_args()

    docs = Path(args.docs_dir)
    history_path = docs / "perf-history" / "history.jsonl"
    html_path = docs / "perf-history" / "index.html"
    history_path.parent.mkdir(parents=True, exist_ok=True)

    history: list[dict] = []
    if history_path.exists():
        for line in history_path.read_text().splitlines():
            line = line.strip()
            if line:
                try:
                    history.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    if not args.render_only:
        if not args.prometheus_file:
            print("--prometheus-file is required unless --render-only is set", file=sys.stderr)
            sys.exit(1)
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
            val = extract_metric(prom_values, m["metric"], m.get("label"))
            record["metrics"][m["key"]] = val

        history.append(record)
        with history_path.open("a") as fh:
            fh.write(json.dumps(record) + "\n")

        # Write one badge per metric
        for m in METRICS:
            val = record["metrics"].get(m["key"], 0.0)
            badge_path = history_path.parent / f"badge-{m['key']}.json"
            badge_path.write_text(json.dumps(badge_json(m["key"], val, m["unit"], m["decimals"])))
            print(f"badge: {badge_path} — {val}")

        print(f"appended record to {history_path} ({len(history)} total)")

    render_html(history, html_path)
    print(f"rendered trend page to {html_path}")


if __name__ == "__main__":
    main()
