# hsa-snoop Limitations and Notes

* **Sampling snoop, not a tap.** Activity is captured by polling the ring. A
  packet that is enqueued *and* queue-consumed entirely within one poll interval
  can be missed (the slot may read back as `INVALID` once consumed). Lower
  `--poll-us` to catch faster kernels; deeply-queued workloads are captured
  near-completely (195/200 in the bundled test). Missed packets are counted
  internally as "dropped".
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
* **Physical addresses** resolve for host/GTT-backed rings (the norm for AQL
  host↔GPU queues). VRAM-resident pages have no pagemap PFN and report `0`.
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
