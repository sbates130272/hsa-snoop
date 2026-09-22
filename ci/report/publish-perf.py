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
import html
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
        if val is None:
            badge_path.write_text(json.dumps(no_data_badge_json(m["key"])))
            continue
        badge_path.write_text(json.dumps(
            badge_json(m["key"], val, m["unit"], m["decimals"])))


def render_html(history: list[dict], out_path: Path) -> None:
    """Write a minimal Chart.js trend page from the history records."""
    keys = [m["key"] for m in METRICS]
    labels_js = json.dumps([
        "{} · {}".format(
            r.get("timestamp", "")[:10],
            r.get("meta", {}).get("sha", "")[:8] or "unknown",
        )
        for r in history
    ])
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
    metric_cards = []
    if history:
        latest = history[-1]
        latest_metrics = latest.get("metrics", {})
        latest_meta = latest.get("meta", {})
        latest_details = (
            f"<p class=\"lede\">Latest run: "
            f"<code>{html.escape(latest_meta.get('sha', '')[:8] or 'unknown')}</code> "
            f"on {html.escape(latest.get('timestamp', 'unknown'))}"
            f" ({html.escape(latest_meta.get('runner', 'unknown'))})</p>"
        )
        for m in METRICS:
            val = latest_metrics.get(m["key"])
            if val is None:
                display = "no data"
            elif m["decimals"] == 0:
                display = f"{int(val):,} {m['unit']}"
            else:
                display = f"{val:.{m['decimals']}f} {m['unit']}"
            metric_cards.append(
                "<article class=\"metric-card\">"
                f"<h2>{html.escape(m['key'].replace('_', ' '))}</h2>"
                f"<p class=\"value\">{html.escape(display)}</p>"
                f"<p>{html.escape(m['description'])}</p>"
                "</article>"
            )
    else:
        latest_details = (
            "<p class=\"lede\">No published history yet. The first successful "
            "default-branch hardware run will append a record here.</p>"
        )
        for m in METRICS:
            metric_cards.append(
                "<article class=\"metric-card\">"
                f"<h2>{html.escape(m['key'].replace('_', ' '))}</h2>"
                "<p class=\"value\">no data</p>"
                f"<p>{html.escape(m['description'])}</p>"
                "</article>"
            )
    rows = []
    for record in reversed(history[-10:]):
        meta = record.get("meta", {})
        cells = [
            f"<td>{html.escape(record.get('timestamp', ''))}</td>",
            f"<td><code>{html.escape(meta.get('sha', '')[:8] or 'unknown')}</code></td>",
            f"<td>{html.escape(meta.get('runner', 'unknown'))}</td>",
        ]
        for m in METRICS:
            val = record.get("metrics", {}).get(m["key"])
            cells.append(
                f"<td>{'' if val is None else html.escape(str(int(val) if m['decimals'] == 0 else round(val, m['decimals'])) )}</td>"
            )
        rows.append("<tr>{}</tr>".format("".join(cells)))
    if rows:
        recent_runs = (
            "<h2>Recent runs</h2>"
            "<div class=\"table-wrap\"><table><thead><tr>"
            "<th>timestamp</th><th>sha</th><th>runner</th>"
            + "".join(f"<th>{html.escape(m['key'])}</th>" for m in METRICS)
            + "</tr></thead><tbody>"
            + "".join(rows)
            + "</tbody></table></div>"
        )
    else:
        recent_runs = ""
    html_doc = f"""\
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>hsa-snoop performance history</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4/dist/chart.umd.min.js"></script>
<style>
  :root {{ color-scheme: light dark; }}
  body {{ font-family: sans-serif; max-width: 1100px; margin: 2em auto; padding: 0 1em 3em; line-height: 1.5; }}
  h1 {{ font-size: 1.8em; margin-bottom: 0.3em; }}
  h2 {{ font-size: 1.1em; }}
  code {{ font-size: 0.95em; }}
  a {{ color: #0b57d0; }}
  .lede {{ margin-bottom: 1.5em; }}
  .chart-wrap {{ position: relative; height: 360px; margin: 2em 0; }}
  .metrics {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(220px, 1fr)); gap: 1rem; margin: 1.5em 0 2em; }}
  .metric-card {{ border: 1px solid #d0d7de; border-radius: 12px; padding: 1rem; background: rgba(127, 127, 127, 0.06); }}
  .metric-card h2, .metric-card p {{ margin: 0; }}
  .metric-card .value {{ font-size: 1.4em; font-weight: 700; margin: 0.35rem 0 0.5rem; }}
  .table-wrap {{ overflow-x: auto; }}
  table {{ border-collapse: collapse; width: 100%; }}
  th, td {{ border-bottom: 1px solid #d0d7de; padding: 0.55rem 0.7rem; text-align: left; white-space: nowrap; }}
</style>
</head>
<body>
<h1>hsa-snoop performance history</h1>
<p>Each point is one CI run on the default branch.  Counters are totals from
a fixed workload duration; higher is more activity observed.</p>
{latest_details}
<section class="metrics">
{"".join(metric_cards)}
</section>
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
{recent_runs}
<p><small>Generated by <code>ci/report/publish-perf.py</code>. Source: <a href="https://github.com/sbates130272/hsa-snoop">sbates130272/hsa-snoop</a>.</small></p>
</body>
</html>
"""
    out_path.write_text(html_doc)


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

        print(f"appended record to {history_path} ({len(history)} total)")

    write_badges(history, history_path.parent)
    render_html(history, html_path)
    print(f"wrote badges to {history_path.parent}")
    print(f"rendered trend page to {html_path}")


if __name__ == "__main__":
    main()
