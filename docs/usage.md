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

The scan attaches to each target process with `PTRACE_SEIZE` and briefly
enables KFD debug mode on it (milliseconds), because the driver only answers
the queue snapshot for a process's tracer. The process keeps running while it
is queried. To let go of it again hsa-snoop has to stop it for an instant,
since the kernel only detaches a stopped process; once the scan has finished,
`TracerPid` in `/proc/<pid>/status` is back to 0 and the process can be
debugged, signalled and reaped as usual. A process that is already being
traced, by a debugger for instance, cannot be attached and is skipped. The
scan requires `CAP_SYS_PTRACE` (root), which hsa-snoop already requires. KFD
ioctl minor version ≥ 1.13 is required (Linux ~6.1 / AMDGPU ~5.7).

That instant has one side effect. It is the main thread of the process that
is stopped, and if that thread is blocked in a system call the kernel does
not restart on its own (`epoll_wait`, `semop`, `msgrcv`, a socket read with
a receive timeout, among others), the call returns `EINTR` although no
signal was delivered. Python and most event-loop libraries retry such calls.
For a workload that treats `EINTR` as an error, start hsa-snoop with
`--no-scan`: nothing is attached or stopped, and only queues created after
hsa-snoop started are traced.

## Trace Files in `--all` Mode

In `--all` mode without `--prometheus`, hsa-snoop keeps one trace per queue
and writes it to `--out-dir` (default `/var/log/hsa-snoop`) when the queue
goes away, that is when its process exits or destroys it. Traces of queues
that are still live are written when hsa-snoop exits.
Each file is named `<command>-<uid>.pftrace` (or `.json`), where `<command>`
is the name of the process that owns the queue and `<uid>` is the queue `uid`
shown in the `[queue]` log lines, for example `gfx-test-3.json`. The name is
announced when the queue is found (`new trace → ...`) and again when the file
is written. Characters in the process name other than letters, digits, `.`,
`_`, `-` and `+` are replaced with `_`.

## Probes Left by a Killed hsa-snoop

hsa-snoop removes its kprobes when it exits, including on `SIGINT` and
`SIGTERM`. One that is killed with `SIGKILL` cannot, and its probes
(`hsasnoop_cq_<pid>`, and with `--ais-snoop` also `hsasnoop_ais_e_<pid>`,
`hsasnoop_ais_r_<pid>` and the tracefs instance `hsa_ais_<pid>`) stay
installed. The next hsa-snoop to start removes every such probe whose `<pid>`
is no longer a running hsa-snoop, and logs each removal. Probes that belong to
an hsa-snoop that is still running are left alone.

## Poll Threads (`--poll-threads`)

The rings are polled by several threads: as many as there are CPU cores, up to 4,
unless `--poll-threads <n>` (1 to 64) says otherwise. A queue is assigned by
the pid of its process, so all queues of one process are polled by the same
thread and its records keep their order. Records of different processes can
reach the dispatch log in a different order than they were submitted in;
each line carries its own timestamp.

The point of more than one thread is isolation. A read of a process's memory
can block in the GPU driver when the GPU is out of memory, and nothing else
is polled on that thread until it returns. With one thread that is every
process; with four it is a quarter of them. Each thread polls at `--poll-us`
while any of its queues is busy, so CPU use under load grows with the number
of threads in use. Idle queues do not cost more with more threads: a thread
with no queues sleeps, and so does one whose queues are all idle, while one
extra thread looks at the idle queues of all of them every `--poll-idle-us`
and wakes the owner of any that moves. That extra thread is shared, so it
is supervised by a timer: if a read blocks it and it has not come round for
a few idle intervals, every poll thread goes back to polling its own idle
queues until the read returns.

`--poll-threads 1` is the behaviour of releases up to 1.9.0. When a poll
thread is held up, hsa-snoop says so in its log and, in Prometheus mode, in
`hsa_poller_stalled_seconds_total` and `hsa_queues_unobserved_tail_total`;
see "Poller health" in the Prometheus page.

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
