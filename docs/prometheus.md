# Prometheus Exporter

Requires `-DHSA_SNOOP_PROMETHEUS=ON` at build time.

```
sudo ./hsa-snoop --all --prometheus                  # serve on default port 9488
sudo ./hsa-snoop --all --prometheus-port 9100        # custom port
curl http://localhost:9488/metrics
```

## HSA / GPU Metrics

All metrics carry constant labels `host`, `platform`, `rocm_version`,
`discovery_mode`.

Options that shape the labels and the series:

| Option | Environment | Effect |
| --- | --- | --- |
| `--host-label <s>` | `HSA_SNOOP_HOST_LABEL` | Value of `host`. Defaults to the host name, which in a container is the container id and changes on every recreate; set it to the node name there. |
| `--rocm-version <s>` | `HSA_SNOOP_ROCM_VERSION` | Value of `rocm_version`. Defaults to the version found under `/opt/rocm`, or `unknown` when ROCm is installed only in the traced containers. |
| `--gpu-clock-hz <hz>` | `HSA_SNOOP_GPU_CLOCK_HZ` | GPU clock rate used for `hsa_kernel_gpu_duration_seconds`. Defaults to a rate measured through KFD. |
| `--kernel-name-max <n>` | — | Truncate `kernel_name` values to `n` bytes (ending in `...`). Default 0, no limit. Names that differ only past the cut share one series. |
| `--poll-threads <n>` | — | Number of ring poll threads, 1 to 64. Default: the number of CPU cores, at most 4. All queues of a process are polled by one thread. See [Poller health](#poller-health). |
| `--series-retain-sec <n>` | `HSA_SNOOP_SERIES_RETAIN_SEC` | How long the series of a process outlive its last queue. Default 300; 0 keeps them for ever. See [Series lifecycle](#series-lifecycle). |

`hsa_kernel_duration_seconds` is sampled. `hsa_kernel_launches_total` carries
`kernel_name` and the unmeasured counter does not, so aggregate both sides to
get the sample rate:

```promql
1 - sum by (gpu_id, pid, comm) (rate(hsa_kernel_durations_unmeasured_total[5m]))
  / sum by (gpu_id, pid, comm) (rate(hsa_kernel_launches_total[5m]))
```

| Metric | Type | Labels | Description |
| --- | --- | --- | --- |
| `hsa_snoop_up` | Gauge | — | 1 when the exporter is running |
| `hsa_kernel_launches_total` | Counter | `kernel_name`, `gpu_id`, `gpu_type`, `pid`, `comm` | Kernel dispatch count. Vendor packets that are not kernel launches are not counted |
| `hsa_kernel_duration_seconds` | Histogram | `kernel_name`, `gpu_id`, `gpu_type` | Kernel duration (host-observed, enqueue to queue-consumed) |
| `hsa_kernel_gpu_duration_seconds` | Histogram | `kernel_name`, `gpu_id`, `gpu_type` | Kernel duration from the command processor's own timestamps. Only for workloads run with queue profiling on (`GPU_FORCE_QUEUE_PROFILING=1` for HIP) |
| `hsa_gpu_clock_ticks_per_second` | Gauge | `gpu_id`, `gpu_type` | GPU clock rate used for that conversion |
| `hsa_kernel_durations_unmeasured_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm` | Launches with no duration observation (see the sample-rate query below) |
| `hsa_kernel_launches_per_second` | Gauge | `gpu_id`, `gpu_type` | Rolling 10 s launch rate |
| `hsa_last_kernel_launch_timestamp_seconds` | Gauge | `gpu_id`, `gpu_type` | Wall-clock time of most recent launch |
| `hsa_last_kernel_launch_info` | Gauge | `gpu_id`, `gpu_type`, `kernel_name` | Most recently launched kernel name (value always 1) |
| `hsa_active_queues` | Gauge | `gpu_id`, `gpu_type` | AQL queues being polled now. Goes down when a queue is destroyed or its process exits |
| `hsa_queue_info` | Gauge | `gpu_id`, `gpu_type`, `pid`, `comm`, `ring_base`, `ring_phys`, `ring_size` | One series per queue being polled now (value always 1). Removed as soon as the queue is dropped |
| `hsa_barrier_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm` | Barrier AQL packets seen |
| `hsa_sdma_copies_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | SDMA copy packets, by direction (`h2d`/`d2h`/`d2d`) |
| `hsa_sdma_bytes_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | Bytes moved by SDMA copies |
| `hsa_sdma_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `opcode` | SDMA packets by opcode |
| `hsa_active_sdma_queues` | Gauge | `gpu_id`, `gpu_type` | SDMA queues being polled now |
| `hsa_sdma_present` | Gauge | — | 1 when first SDMA queue seen; 0 on APU/unified-memory nodes |
| `hsa_triggered` | Gauge | `gpu_id`, `gpu_type` | 1 on first kernel dispatch on this GPU |
| `hsa_errors_total` | Counter | `gpu_id`, `pid` | Packets before queue registration |
| `hsa_dispatches_missed_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `reason` | AQL packets not decoded. `reason` is `overwritten` (the ring wrapped before they were read), `consumed` (already consumed and the slot no longer holds a packet that can be decoded) or `unreadable` (the slot read failed). Counts every packet type, not only kernel dispatches |
| `hsa_poller_stalled_seconds_total` | Counter | `thread` | Time a poll thread was held up with queues overdue for a poll. See [Poller health](#poller-health) |
| `hsa_queues_unobserved_tail_total` | Counter | `gpu_id`, `gpu_type`, `kind` | Queues (`kind` is `aql` or `sdma`) that went away too long after their last successful poll, or without ever being read. What they did in that time is in no other counter |
| `hsa_xnack_total` | Counter | `pasid`, `comm` | GPU page-fault retries (`--xnack-snoop`) |

Per-GPU totals of the per-process counters above. They count the same events
across all processes and are never removed:

| Metric | Labels | Total of |
| --- | --- | --- |
| `hsa_gpu_kernel_launches_total` | `gpu_id`, `gpu_type` | `hsa_kernel_launches_total` |
| `hsa_gpu_kernel_durations_unmeasured_total` | `gpu_id`, `gpu_type` | `hsa_kernel_durations_unmeasured_total` |
| `hsa_gpu_dispatches_missed_total` | `gpu_id`, `gpu_type`, `reason` | `hsa_dispatches_missed_total` |
| `hsa_gpu_barrier_packets_total` | `gpu_id`, `gpu_type` | `hsa_barrier_packets_total` |
| `hsa_gpu_sdma_copies_total` | `gpu_id`, `gpu_type`, `direction` | `hsa_sdma_copies_total` |
| `hsa_gpu_sdma_bytes_total` | `gpu_id`, `gpu_type`, `direction` | `hsa_sdma_bytes_total` |
| `hsa_gpu_sdma_packets_total` | `gpu_id`, `gpu_type`, `opcode` | `hsa_sdma_packets_total` |

## Series lifecycle

Every series with a `pid` label belongs to one process. When the last queue
of that process is gone (it exited, or shut its runtime down), its series are
kept for `--series-retain-sec` seconds, 300 by default, and then removed from
`/metrics`. Without this a node where jobs come and go would export a few more
series for every process it has ever seen, and hsa-snoop's memory would grow
with them.

- **Set the retention to at least two scrape intervals.** A series has to be
  scraped once after its last change for Prometheus to hold its final value.
- **The retention starts over if the process comes back.** A new queue of the
  same pid inside the period keeps the series and their counts. After the
  period a reused pid starts new series from 0, which `rate()` and
  `increase()` handle as a counter reset.
- **`hsa_queue_info` is not retained.** It is removed with its queue, so a
  queue that lives for less than a scrape interval may never be scraped. The
  `queue` events of `--log-dispatches` record every queue.
- **AIS operation counts** (`ais_rx_ops_total`, `ais_tx_ops_total`) carry a
  `pid` too. For a process with no queue (one outside `--pid`, or a thread
  other than the main one, which AIS reports under its own id) they are kept
  for as long as the process or thread exists, however long it goes between
  two operations, and removed the retention period after it is found gone.
- **`--series-retain-sec 0`** keeps every series for as long as hsa-snoop
  runs, as releases up to 1.9.0 did. The two active-queue gauges and
  `hsa_queue_info` follow the live queues all the same.

What this means for counters: a per-process counter is not monotonic for the
life of hsa-snoop, because it disappears. Summing it over processes, as in
`sum by (gpu_id) (hsa_kernel_launches_total)`, gives a value that drops each
time a process's series are removed. `sum(rate(...))` and `sum(increase(...))`
are not affected, since Prometheus computes them per series before summing.
For a total that only ever goes up, use the `hsa_gpu_*_total` counters. The
histograms, the per-GPU gauges and the `--mem-snoop` metrics have no `pid`
label and are never removed.

## Poller health

hsa-snoop sees a queue only when it polls it. Two counters say when it could
not, so that a gap in the other metrics is not mistaken for a quiet GPU.
Neither has a `pid` label and neither is ever removed.

- **`hsa_poller_stalled_seconds_total`** has one series per poll thread
  (`--poll-threads`). It grows by the time a thread got to a queue later than
  the queue was due, once that is more than 50 ms (or more than the queue's
  own interval, for an idle queue polled less often than that). Time during
  which several queues of a thread were overdue is counted once. A thread is
  normally held up by a read of a process's memory that blocks in the kernel,
  which happens when a GPU is out of memory; none of that thread's queues is
  looked at meanwhile. If the queue being read goes away during such a read,
  the time the thread was held in it is counted as well, even when the
  thread has no other queue left. Launches made during a stall are still decoded
  afterwards as long as the queue is still there and its ring has not
  wrapped, but their durations are wrong or unmeasured.
- **`hsa_queues_unobserved_tail_total`** counts queues whose end was not
  seen: the queue went away more than about 50 ms after its last successful
  poll (more for an idle queue with a longer poll interval), or was never
  read at all. The time is taken up to the read that failed, so what
  hsa-snoop then spends finding out why is not held against the queue. Whatever the queue did in that time is lost, and no `missed`
  counter includes it, because the amount is not known. A process usually
  has two or three queues, and each counts.

Both say where to distrust the other numbers, not how much is missing: a
queue counted here may have done nothing in the time it was not watched. In
the overload test described in the limitations page every process that lost
launches had a queue counted here, and so did most that lost none. Both stay
at 0 on a system that is keeping up, so alert on any increase of the second
and on a sustained rate of the first:

```promql
increase(hsa_queues_unobserved_tail_total[10m]) > 0
sum(rate(hsa_poller_stalled_seconds_total[5m])) > 0.05
```

What to do when they grow: run fewer processes per GPU or give the GPU more
memory, so that reads stop blocking; and raise `--poll-threads`, so that one
blocked read holds up fewer processes. See the limitations page for what was
measured.

## AIS Metrics

All require `--ais-snoop`. Labels: `gpu_id`, `gpu_type`, `pcie_id`,
`device_type`, `vendor`.

| Metric | Type | Description |
| --- | --- | --- |
| `ais_snoop_armed` | Gauge | 1 if AIS kprobe started; absent if `--ais-snoop` not passed |
| `ais_active` | Gauge | 1 on first AIS operation |
| `ais_rx_ops_total` / `ais_tx_ops_total` | Counter | AIS ioctls completed |
| `ais_rx_bytes_total` / `ais_tx_bytes_total` | Counter | P2P DMA bytes transferred |
| `ais_rx_errors_total` / `ais_tx_errors_total` | Counter | Failed operations (extra `errno` label) |
| `ais_rx_latency_seconds` / `ais_tx_latency_seconds` | Histogram | ioctl entry-to-return latency |
| `ais_rx_io_size_bytes` / `ais_tx_io_size_bytes` | Histogram | Requested transfer size |
| `ais_pcie_device_info` | Gauge | Info metric per PCIe device |
