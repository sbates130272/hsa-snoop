# Grafana Dashboard

[![Grafana Dashboard](https://sbates130272.github.io/hsa-snoop/images/grafana-dashboard.png)](https://github.com/sbates130272/hsa-snoop/blob/main/deploy/grafana/hsa-snoop-dashboard.json)

> Screenshot updated automatically by the `grafana-screenshot` CI workflow after
> each successful hardware-test run.

`deploy/grafana/hsa-snoop-dashboard.json` reads from two datasources:

- **Prometheus** — Overview, Throughput, Duration, Queue, SDMA and AIS rows.
  Requires `--prometheus`.
- **Loki** — Kernel Launch Timeline and hsa-snoop Logs rows. Requires
  `--log-dispatches` shipped into Loki via `scripts/observability/`.

The Prometheus rows work standalone; Loki rows stay empty without a log
pipeline. Import the dashboard via Grafana's **Dashboards → Import**.

## Dashboard Development Workflow

Iterating on a dashboard means many minutes of clicking, and holding a
`--gres=gpu:1` Slurm allocation open for that is wasteful. The workflow is
therefore capture-then-replay:

```
# 1. On a GPU node under Slurm: run a phased workload with the exporter and the
#    dispatch log on, then freeze a Prometheus TSDB snapshot plus the logs.
scripts/slurm/submit-obs.sh my-run

# 2. On the login node, with no allocation: replay them into a local
#    Grafana + Prometheus + Loki stack.
scripts/observability/stack.sh up results/my-run

# 3. Edit deploy/grafana/hsa-snoop-dashboard.json, then re-provision without
#    re-ingesting anything.
scripts/observability/stack.sh reload

# 4. Assert that every panel actually returns data.
scripts/observability/check-dashboard.py
scripts/observability/stack.sh down
```

`check-dashboard.py` walks the dashboard JSON, expands template and interval
variables, and runs every target against the replayed datasources. This catches
panels that are silently empty due to renamed labels or missing metrics.
