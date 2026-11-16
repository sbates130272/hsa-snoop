# Performance Tracker

Each point is one hardware-test run on `main`: a fixed workload against
an emulated GPU, traced with `hsa-snoop --prometheus`. The values are
counter totals for that workload, so a change means the tool observed
more or less activity than it did before, not that anything ran faster.

<p id="perf-latest" class="perf-empty">Loading the published history…</p>

<div id="perf-cards" class="perf-cards" markdown="0"></div>

## Trends

<div id="perf-charts" class="perf-charts" markdown="0"></div>

## Recent Runs

<div id="perf-runs" markdown="0"></div>

## Where The Numbers Come From

The `vm-hardware-test` workflow scrapes the exporter at the end of its
run and hands the exposition text to
`.github/ci/report/publish-perf.py`, which appends one record to
`perf/history.jsonl` on the `gh-pages` branch and refreshes the
`perf/badge-*.json` files behind the README badges. Nothing in `main`
carries performance data; this page fetches the history when it loads.

The same run publishes every metric it exported, not only the four
tracked here, to the [Prometheus snapshot](../prom/index.md).
Per-kernel durations from that run are on the
[kernel timing](../timing/index.md) page.
