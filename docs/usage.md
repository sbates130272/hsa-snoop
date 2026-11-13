# hsa-snoop Usage Guide

## WSL2 / librocdxg Mode

On Windows Subsystem for Linux 2, the amdgpu KFD device is not present.
Instead, AMD's librocdxg library bridges HSA calls through the Windows DXG
kernel interface. hsa-snoop detects WSL2 automatically and switches to
bpftrace mode:

```
sudo ./hsa-snoop -- ./my_hip_app     # auto-selects bpftrace on WSL2
```

Or force it explicitly on any system:

```
sudo ./hsa-snoop --mode bpftrace -- ./my_hip_app
sudo ./hsa-snoop --mode bpftrace --all
```

In bpftrace mode, hsa-snoop forks a `bpftrace` child that attaches a uprobe
to `hsaKmtCreateQueueExt` in `librocdxg.so`. The library path is
auto-detected via `/opt/rocm/lib/librocdxg.so`; pass `--librocdxg <path>` to
override. `bpftrace` must be installed and the process must run as root.

## Startup Scan for Pre-existing Queues

hsa-snoop automatically scans all running KFD processes at startup using the
KFD debug-trap API (`AMDKFD_IOC_DBG_TRAP / KFD_IOC_DBG_TRAP_GET_QUEUE_SNAPSHOT`).
This discovers queues that were created before hsa-snoop started, with full
metadata (ring VA, wptr, rptr, gpu_id, queue type). The scan runs in launch
mode is skipped (the target process hasn't started yet); in `--pid` and `--all`
modes it runs automatically before the event-driven probe is armed.

The scan uses `PTRACE_SEIZE` (non-stopping) and briefly enables KFD debug mode
on each target process (milliseconds). It requires `CAP_SYS_PTRACE` (root),
which hsa-snoop already requires. KFD ioctl minor version ≥ 1.13 is required
(Linux ~6.1 / AMDGPU ~5.7).

## Dispatch Event Log (`--log-dispatches`)

Prometheus is an aggregating store sampled at the scrape interval. At the
~2700 kernel launches/s a tight HIP loop reaches, a 1 s scrape collapses
thousands of distinct dispatches into a single counter delta, and no dashboard
built on it can show *which kernel ran when*. `--log-dispatches` is the other
half: one NDJSON line per event, with a wall-clock RFC3339 timestamp, written
independently of both the trace files and the exporter.

```
sudo ./hsa-snoop --all --prometheus --log-dispatches /var/log/hsa-snoop/events.ndjson
sudo ./hsa-snoop -- ./my_hip_app --log-dispatches -      # '-' means stderr
```

```json
{"ts":"2026-09-01T22:04:28.106365Z","ev":"queue","kind":"aql","gpu_id":10976,"pid":1795691,"comm":"gfx-test","queue_uid":1,"qtype":2,"ring_size":4096,"ring_va":"0x7097172a9000","ring_phys":"0x63f49cc000"}
{"ts":"2026-09-01T22:04:28.392511Z","ev":"dispatch","gpu_id":10976,"pid":1795691,"comm":"gfx-test","queue_uid":2,"dispatch_id":0,"kernel":"vadd(float const*, float const*, float*, int)","grid":[26624,1,1],"wg":[256,1,1],"lds":0,"scratch":0,"dur_us":812.500}
```

`ev` is one of `queue`, `dispatch` or `sdma`. `dur_us` is present only when the
queue was observed to retire the packet; 64-bit addresses are hex **strings**,
because JSON numbers are IEEE doubles in most consumers and would silently lose
the low bits. Available in every build — it does not require
`-DHSA_SNOOP_PROMETHEUS=ON`.

This is the stream the Grafana kernel-launch timeline is built from; ship it
into Loki with the Alloy config under `scripts/observability/`.

> [!NOTE]
> The log is written unbuffered-per-line from the parser sink thread. At a
> sustained few thousand dispatches/s it produces on the order of 15 MB/minute,
> so point it at local storage rather than NFS for long runs.

## Quick Discovery-Only Check (No Build)

```
sudo bpftrace scripts/hsa-snoop.bt
```

## Running as a systemd Daemon

Requires a build with `-DHSA_SNOOP_PROMETHEUS=ON`.

```bash
# Configure with the same install prefix you intend to deploy.
cmake -B build -DHSA_SNOOP_PROMETHEUS=ON -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel

# Install binary and unit files to the prefix chosen above
sudo cmake --install build

sudo systemctl daemon-reload
sudo systemctl enable --now hsa-snoop-prometheus

# Check status / live logs
sudo systemctl status hsa-snoop-prometheus
sudo journalctl -fu hsa-snoop-prometheus

# Verify the metrics endpoint
curl http://localhost:9488/metrics
```

The packaged/systemd-managed daemon is `hsa-snoop-prometheus.service`.
Prometheus-enabled installs also stage `/etc/default/hsa-snoop`; edit it to tune
the daemon (poll interval, optional snoops, etc.), then run
`sudo systemctl restart hsa-snoop-prometheus.service`.

To change the port, set it in `HSA_SNOOP_ARGS` or override `ExecStart` in a
drop-in:

```bash
sudo mkdir -p /etc/systemd/system/hsa-snoop-prometheus.service.d
cat <<EOF | sudo tee /etc/systemd/system/hsa-snoop-prometheus.service.d/port.conf
[Service]
ExecStart=
ExecStart=/usr/local/bin/hsa-snoop --all --prometheus --prometheus-port 9100
EOF
sudo systemctl daemon-reload && sudo systemctl restart hsa-snoop-prometheus
```

## Grafana Dashboard Development

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

`check-dashboard.py` walks the dashboard JSON, expands Grafana's template and
interval variables, and runs every target against the replayed datasources.
Loading a dashboard in a browser tells you it renders; this tells you which
panels are silently empty because a label was renamed or a metric is only
created after the first error.
