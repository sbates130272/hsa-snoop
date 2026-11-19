# hsa-snoop Limitations and Notes

* **Sampling snoop, not a tap.** Activity is captured by polling the ring. A
  packet that is enqueued *and* queue-consumed entirely within one poll interval
  can be missed (the slot may read back as `INVALID` once consumed). Lower
  `--poll-us` to catch faster kernels; deeply-queued workloads are captured
  near-completely (195/200 in the bundled test). Missed packets are exported
  as `hsa_dispatches_missed_total` in Prometheus mode, split by `reason`. The
  count covers every AQL packet type, so `launches / (launches + missed)` is a
  lower bound on the capture ratio for kernel dispatches.
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
* **Attach mode only sees queues created after attach**, since the kprobe fires
  on `create_queue`. Use launch mode to guarantee capture from process start.
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
* **Physical addresses** resolve for host/GTT-backed rings. VRAM-resident pages
  have no pagemap PFN and report `0`.
* **Rings that `process_vm_readv` cannot read** are read through
  `/proc/<pid>/mem` instead, and `ring_phys` is `0x0` for them. Two cases are
  known on ROCm 10.2. On gfx94x and gfx125x with Large BAR, AQL rings are
  placed in device memory by default; `DEBUG_CLR_AQL_DEV_QUEUE=0` in the traced
  process restores host rings there. On gfx1201 the rings are also unreadable
  with `process_vm_readv`, that variable has no effect, and the cause is not
  yet known.
* **SDMA decode supports v4.x, v6.x, and v7.x packet layouts.** v4 covers gfx9
  CDNA (gfx90a / gfx942); v6 covers gfx11 RDNA3 (including gfx1101); v7 covers
  gfx12 (including gfx1250 / MI455X). Unknown generations are ignored to avoid
  losing ring synchronisation. Non-linear copy layouts (tiled, sub-window) are
  counted and stepped over but do not yet contribute a byte total.
* **SDMA copy direction is a pagemap heuristic.** A src/dst page with no PFN is
  treated as device (VRAM) memory; this also matches a not-present/swapped host
  page, so a copy touching a paged-out host buffer can be misclassified. For
  live, pinned copy buffers (the common case) it is accurate.
* Verified on: gfx90a (MI2xx / Aldebaran), ROCm 7.1.0, amdgpu 6.16 DKMS,
  kernel 6.8, x86-64; gfx1101 SDMA v6 COPY_LINEAR; gfx1250 (MI455X) SDMA v7
  COPY_LINEAR via rocjitsu/vfio-user. SDMA capture calibration is in progress
  on gfx942 (MI3xx).
