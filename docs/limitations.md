# hsa-snoop Limitations and Notes

* **Sampling snoop, not a tap.** Activity is captured by polling the ring. A
  packet that is enqueued *and* queue-consumed entirely within one poll interval
  can be missed (the slot may read back as `INVALID` once consumed). Lower
  `--poll-us` to catch faster kernels; deeply-queued workloads are captured
  near-completely (195/200 in the bundled test). Missed packets are exported
  as `hsa_dispatches_missed_total` in Prometheus mode, split by `reason`. The
  count covers every AQL packet type, so `launches / (launches + missed)` is a
  lower bound on the capture ratio for kernel dispatches.
* **GPU-timestamped durations need queue profiling in the workload.** With
  `GPU_FORCE_QUEUE_PROFILING=1` set for a HIP workload, the command processor
  stamps each dispatch, and `hsa_kernel_gpu_duration_seconds` reports those
  durations for every dispatch whose completion signal was read, however short
  the kernel. Three caveats. The stamps are in GPU clock ticks, whose rate is
  measured through KFD or given with `--gpu-clock-hz`; where neither is
  available the histogram stays empty. The runtime reuses a small pool of
  completion signals, so in a dense burst a dispatch can lose its stamps before
  they are read. And the interval starts when the command processor accepts
  the dispatch, so it can include time queued behind earlier kernels.
* **An idle queue is polled less often.** A queue that has not moved for 10 ms
  is polled at a growing interval, up to `--poll-idle-us` (1000 by default).
  The first packets after an idle spell are therefore seen up to that much
  later, which can cost them their host-observed duration. `--poll-idle-us 0`
  polls every queue at `--poll-us` always.
* **Durations are sampled.** `hsa_kernel_duration_seconds` is only observed for
  a packet the poller saw while it was still pending. A packet that was already
  consumed when first seen is counted as a launch but has no measurable
  duration, so on a workload of very short kernels the histogram count is well
  below `hsa_kernel_launches_total`. Those launches are counted in
  `hsa_kernel_durations_unmeasured_total`, so the sample rate is one minus
  unmeasured over launches (the query is in the Prometheus page); a lower
  `--poll-us` raises it at the cost of CPU.
  The sample is biased towards longer kernels, which are more likely to be seen
  pending.
* **Unresolved kernel names.** A kernel whose descriptor cannot be matched to a
  symbol is labelled `kernel_0x<addr>`. The lookup is retried by up to three
  further scans of the process, at least 30 s apart, so a name that was
  unavailable at first sight is picked up for later launches under a new
  series.
* **`--mem-snoop` reuses a snapshot of the process's VM map for up to one
  second**, because reading `/proc/<pid>/maps` per dispatch stalls the poller on
  processes with thousands of mappings. A buffer mapped within the last second
  may be absent from `mapped_vram` for that dispatch.
* **Timing is host-observed.** enqueue→queue-consumed uses host timestamps for
  queue pointer transitions, so a slice includes queue-wait time and may differ
  from true kernel execution or completion-signal timing. No profiling signals
  or counters are read; this is best used for structure and relative timing.
* **Packets on a queue that already existed are reported from the first poll
  onwards.** A queue seen being created by the KFD `create_queue` probe is
  decoded from the start of its ring. Queues found by the startup scan, and
  queues discovered through `librocdxg` on WSL2, start from the first poll.
  The `[summary]` line printed when a queue stops being polled gives the
  decoded and missed counts, and how much was in the ring before the first
  poll.
* **Attach mode only sees queues created after attach**, since the kprobe fires
  on `create_queue`. Use launch mode to guarantee capture from process start.
* **AIS byte counts are the requested sizes of requests that succeeded.** The
  return probe gives each request's status but not the bytes moved, so a short
  transfer is reported at its requested size. A failed request counts as an
  error and moves nothing.
* **`hsa_kernel_duration_seconds` is a coarse histogram by nature.** Its buckets
  run 50 µs to 5 s on a 1-2.5-5 ladder, chosen against 96,749 measured
  dispatches so the fullest bucket holds ~42% of samples rather than the 72% the
  original decade ladder produced. `histogram_quantile` still interpolates
  inside a bucket, so a p99 is accurate to roughly the bucket width. When an
  exact percentile matters, take it from the `--log-dispatches` stream
  (`quantile_over_time(... | unwrap dur_us ...)`), which quantiles the individual
  measurements. The 50 µs floor is deliberate: durations come from polling the
  ring at `--poll-us` (default 20 µs), so anything below a couple of poll
  intervals is quantisation noise.
* **WSL2 PID mapping is heuristic.** bpftrace reports host-kernel PIDs, which
  differ from WSL2 namespace PIDs by a fixed offset. In `--all` mode without a
  known target PID, hsa-snoop scans `/proc` by comm name to resolve the
  namespace PID. This can misattribute queues if two processes share the same
  comm prefix. Launch mode (`-- <command>`) avoids this by using the known
  child PID.
* **Physical addresses** resolve for rings in ordinary process memory. A ring
  with no frame visible in pagemap reports `ring_phys=0x0`; that is also what
  a run without `CAP_SYS_ADMIN` sees.
* **From ROCm 10.2 every ring is a GPU buffer object.** The runtime allocates
  AQL and SDMA rings as non-paged buffers mapped through the DRM render node,
  where 10.1 used anonymous memory. `process_vm_readv` cannot read such a
  mapping, so hsa-snoop reads it through `/proc/<pid>/mem`, which costs more
  per read. This applies to every GPU, and no runtime setting restores the old
  layout. The `[queue]` line shows what backs each ring as `ring_backing=`.
* **SDMA decode supports v4.x, v6.x, and v7.x packet layouts.** v4 covers gfx9
  CDNA (gfx90a / gfx942); v6 covers gfx11 RDNA3 (including gfx1101); v7 covers
  gfx12. Up to gfx12.4 the runtime writes the v6 packet set; from gfx12.5
  (gfx1250 / MI455X) it writes variable-length copies with optional wait and
  signal blocks and 64-bit fence and poll packets, which are decoded as such.
  Unknown generations are ignored to avoid losing ring synchronisation. A
  packet the decoder does not recognise makes it skip to the write pointer;
  the `[summary]` line reports the skipped amount, which should be zero.
  Non-linear copy layouts (tiled, sub-window) and copies to several destinations are counted
  and stepped over but do not yet contribute a byte total.
* **SDMA copy direction is a pagemap heuristic.** A src/dst page with no PFN is
  treated as device (VRAM) memory; this also matches a not-present/swapped host
  page, so a copy touching a paged-out host buffer can be misclassified. For
  live, pinned copy buffers (the common case) it is accurate.
* Verified on: gfx90a (MI2xx / Aldebaran), ROCm 7.1.0, amdgpu 6.16 DKMS,
  kernel 6.8, x86-64; gfx1101 SDMA v6 COPY_LINEAR; gfx1250 (MI455X) SDMA v7
  COPY_LINEAR via rocjitsu/vfio-user. SDMA capture calibration is in progress
  on gfx942 (MI3xx).
