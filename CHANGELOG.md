# Changelog

All notable changes to hsa-snoop are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.10.1] — 2026-11-24

### Fixed
- `.deb` no longer ships `prometheus-cpp` headers and static libraries.
  `FetchContent_MakeAvailable` does not propagate `EXCLUDE_FROM_ALL`, so
  switching to `FetchContent_Populate` + `add_subdirectory(… EXCLUDE_FROM_ALL)`
  prevents prometheus-cpp's install rules from running during `cmake --install`.
- `postinst` and `prerm` no longer print systemctl errors about the non-existent
  `hsa-snoop.service` unit on install, upgrade, or removal.

## [1.10.0] — 2026-11-23

### Added
- `--series-retain-sec <n>` (default 300, env `HSA_SNOOP_SERIES_RETAIN_SEC`):
  in Prometheus mode the series of a process, that is every series with a
  `pid` label, are removed that many seconds after its last queue went away.
  0 keeps them for ever, as before. A process that comes back inside the
  period keeps its series.
- `--poll-threads <n>` (1 to 64; default the number of CPU cores, at most 4): the
  rings are polled by several threads. All queues of a process are polled by
  one thread, so a read that blocks in the GPU driver holds up only the
  processes that share its thread. `--poll-threads 1` is the old behaviour.
- `hsa_poller_stalled_seconds_total` (per poll thread) counts the time a poll
  thread was held up with queues overdue, and
  `hsa_queues_unobserved_tail_total` (per GPU) the queues that went away too
  long after their last successful poll for their end to have been seen.
  Time a thread was held in the last poll of a queue that then went away is
  part of the first; time hsa-snoop itself took to find out why a queue had
  become unreadable is not part of the second.
  Without Prometheus the same is in the log: a `[parser] poll thread ...
  stalled` line, a `[parser] queue ... went away` line, and a `[summary]
  poller` line with the totals at exit.
- The per-queue `[summary]` line ends with the number of successful polls,
  the longest time between two polls and `tail_unobserved`. A queue that was
  never read gets a `[summary] unread ... queue` line if it was there long
  enough that it should have been.
- `--no-scan` skips the startup scan for queues that already exist. The scan
  has to stop each KFD process for an instant (see Fixed), and a blocking
  system call that the process's main thread is in at that moment can return
  `EINTR`. Use it for a workload that does not retry such calls; only queues
  created after hsa-snoop started are then seen. It replaces `--kfd-scan` in
  the help text, an option that was listed but never existed.
- Per-GPU totals that are never removed: `hsa_gpu_kernel_launches_total`,
  `hsa_gpu_kernel_durations_unmeasured_total`,
  `hsa_gpu_dispatches_missed_total`, `hsa_gpu_barrier_packets_total`,
  `hsa_gpu_sdma_copies_total`, `hsa_gpu_sdma_bytes_total` and
  `hsa_gpu_sdma_packets_total`. Use these where a counter has to stay
  monotonic across process exits.

### Changed
- Polling uses up to four threads by default instead of one. Under load each
  thread in use polls at `--poll-us`, so hsa-snoop uses more CPU than before
  when many processes are busy (about three times as much with 24 processes
  on one GPU). Idle queues do not cost more: poll threads whose queues are
  all idle sleep, and one extra thread watches those queues for them. If a
  read blocks that extra thread, the poll threads take their idle queues
  back until it returns.
  Records of different processes may reach the dispatch log in a different
  order.
- Idle queues are polled together, at common points in time, and an hsa-snoop
  with no queue at all sleeps instead of waking every millisecond. In the
  test VM that took it from 3.7% to under 0.5% of a CPU with no queue and from
  10.2% to 7.7% with 16 idle queues.
- Per-process counters are no longer exported for the whole life of
  hsa-snoop. A `sum()` over them without `rate()` or `increase()` drops when
  a process's series are removed; see "Series lifecycle" in the Prometheus
  page.
- `hsa_queue_info` has a series only for queues that are being polled. It is
  removed as soon as its queue is dropped.
- In `--all` trace mode the trace of a queue is written when the queue goes
  away, not when hsa-snoop exits, and its memory is released then.
- In `--all` trace mode the files are named `<command>-<uid>`, as announced
  when a queue is found. They used to be written as `queue-<uid>` whatever the
  log had said. Characters of the process name that are not safe in a file
  name are replaced with `_`.
- The `debian-publish` workflow replaces the package of a release that already
  exists and no longer fails on it, and runs for the same commit go one after
  the other. Merging a release and pushing its tag starts two runs that both
  publish the tag; the second one used to end red.
- The KFD scan hardware test checks that the workload is no longer attached
  after the scan and that it can be stopped while hsa-snoop is running.
- The VM hardware test uses `qemu-tool-action` v2 and qemu-tool 3.0.0 in
  place of v1 and a compose stack pinned to an old qemu-minimal commit. The guest is described
  by a fleet XML file, `.github/ci/vm-fleet.xml`: ROCm 10.2, two emulated
  GPUs and one NVMe namespace. The job runs on the runner itself, no longer
  in a container.
- A new `two-gpu-hardware-test` runs one workload on each GPU at the same
  time and checks that every launch is credited to the right `gpu_id`. It
  skips on a system with one GPU unless `-DHSA_SNOOP_HARDWARE_GPUS=2` says two
  are expected, which the VM hardware test does.

### Fixed
- With a GPU out of memory, the output of whole processes could go missing
  with every counter at 0. One read of a process's memory can block in the
  driver for over a minute, the single poll thread polled nothing else
  meanwhile, and a process that exited in that time took its ring with it.
  The loss is now reported (see Added) and, with several poll threads,
  smaller. In six runs each of 24 short processes on one emulated GPU, one
  poll thread reported 75%, 79%, 79%, 79%, 99.7% and 99.9% of the launches
  and four threads 95.8%, 99.8%, 99.96% and three times 100%. Every process
  that lost launches was flagged, as were most that lost none.
- Packets that had been decoded but were still waiting for their completion
  when their queue went away were dropped without a trace. They are now
  reported, as completed if the read pointer had passed them.
- `hsa_active_queues` and `hsa_active_sdma_queues` only ever went up. They
  now go down when a queue is destroyed or its process exits.
- With processes coming and going, the number of exported series and the
  memory of hsa-snoop grew without bound, because nothing kept for a queue or
  a process was released. A 20-minute run with 686 short processes went from
  1,322 to 8,449 series and from 6 to 20 MB.
- A process found by the startup scan stayed attached to hsa-snoop with
  `ptrace` for as long as hsa-snoop ran. Signals sent to it were held back, so
  it could not be stopped, its parent could not reap it and no debugger could
  attach. The scan detached from a process that was still running, which the
  kernel refuses, and did not check the result. It now stops the process for
  an instant first, hands back a signal that arrived meanwhile, and reports a
  detach that fails. The stop is what can make a blocking system call return
  `EINTR`; see `--no-scan`.
- Probes left in tracefs by an hsa-snoop that was killed with `SIGKILL` are
  removed by the next one to start, along with the tracefs instance of
  `--ais-snoop`. Probes of an hsa-snoop that is still running are not touched.
- A workload that exits normally no longer logs `evicting stale queue` for
  each of its queues. The runtime destroys its queues, and removes the
  mapping of their pointers, while the process is still alive; that is now
  recognised and the queue is dropped at the first failed read instead of
  after 50. A process
  that is exiting or is a zombie counts as gone as well. The message is kept
  for pointers that were never readable or are still mapped.

## [1.9.0] — 2026-11-20

### Added
- `hsa_kernel_gpu_duration_seconds`: kernel durations from the command
  processor's own timestamps, for workloads run with queue profiling on
  (`GPU_FORCE_QUEUE_PROFILING=1` for HIP). Unlike the host-observed histogram
  it does not depend on the poller seeing a packet while it is pending.
  `hsa_gpu_clock_ticks_per_second` reports the clock rate used, which is
  measured through KFD or set with `--gpu-clock-hz`.
- `--poll-idle-us`: the longest interval between polls of a queue that has
  stopped moving.
- The `[queue]` line reports what backs the ring as `ring_backing=`.
- The SDMA `[summary]` line reports how much of the ring was still waiting to
  be decoded when polling stopped.

### Changed
- A queue with no activity for 10 ms is polled at a growing interval, up to
  1 ms by default, and returns to `--poll-us` as soon as it moves. Idle CPU
  use drops accordingly. The first packets after an idle spell can be seen up
  to that much later; `--poll-idle-us 0` restores the old behaviour.
- A poll pass makes fewer system calls: both queue pointers are read in one
  call, and whether a process is still alive is checked every 100 ms instead of
  every pass.
- AIS requests are reported with their return status. A failed request is
  counted as an error and no longer adds to the byte totals.
- The AIS hardware tests require all ten requests to succeed, and skip on a
  guest whose CPU vendor is not AMD, where the kernel refuses the peer-to-peer
  path AIS needs.
- The VM hardware test advertises its virtual CPU as AMD whatever the host is.
  On Intel hosts the guest used to inherit the Intel vendor, every AIS request
  failed, and the published AIS byte counts dropped tenfold on those runs.
- The SDMA hardware tests fail when a ring was not fully decoded.

### Fixed
- SDMA copies were dropped on gfx12.5 and later (gfx1250 / MI455X). The runtime
  writes copies there with optional wait and signal blocks, so their length
  varies; the decoder assumed a fixed length, lost its place in the ring and
  skipped to the write pointer. On the test workload 25 to 37 of 41 copies were
  reported, and device-to-host copies were shown as device-to-device. The
  64-bit fence and poll packets and the wider cache-rinse request used there
  are decoded too.
- On gfx12 below gfx12.5 a linear copy was assumed to be eight dwords; the
  runtime writes seven, as on gfx11.
- The AIS monitor discarded requests still buffered when it was stopped, and
  left its tracefs instance behind on exit.
- SDMA swap copies on gfx9.4 and later are decoded; an indirect copy on
  gfx12.5 and later reports the buffers it copies, not the cells that hold
  their addresses.
- A descriptor on `/proc/<pid>/mem` was kept after its process exited, so a
  reused process id could not be read.
- GPU timestamps were read from a completion signal that had timed out, where
  they belong to a later dispatch.

## [1.8.0] — 2026-11-20

### Added
- A `[summary]` line per queue when it stops being polled, with the number of
  packets decoded, the number missed by reason, and how much was already in
  the ring before the first poll.
- The VM hardware test keeps the traces its tests assert on, a second kernel
  timing capture with the debug log on, the AIS workload's own output and the
  guest's AIS kernel messages.

### Changed
- Only kernel dispatches count in `hsa_kernel_launches_total`, the dispatch log
  and `--mem-snoop`. Vendor packets that are not kernel launches, such as
  the internal barrier packets of the runtime, were counted as launches named
  `vendor_specific`; they are still shown in traces.
- A queue seen being created by the KFD `create_queue` probe, AQL or SDMA, is
  decoded from the start of its ring. Polling used to start from the write
  pointer at the first poll, which skipped anything enqueued before then.
  Queues found by the startup scan, and queues discovered through `librocdxg`
  on WSL2, still start from the first poll.
- The VM hardware test runs on a guest with ROCm 10.2 and Linux 7.2.6
  (previously ROCm 10.0 and 7.2.4).

### Fixed
- An extended kernel dispatch seen only after it was consumed was decoded with
  the classic packet layout and reported a wrong grid size.
- A barrier packet seen only after it was consumed could be reported as a
  kernel launch.

## [1.7.0] — 2026-11-19

### Added
- `hsa_dispatches_missed_total` counter: AQL packets that were never decoded,
  labelled by `reason`, so a consumer can see loss and tell a poller that is
  behind from packets that could not be decoded.
- Startup warning when process ids from the `create_queue` probe are not
  visible in `/proc` (container outside the host PID namespace).
- `hsa_kernel_durations_unmeasured_total` counter: launches that produced no
  duration observation, which exposes the sample rate of
  `hsa_kernel_duration_seconds`.
- `--host-label` and `--rocm-version` (also `HSA_SNOOP_HOST_LABEL` and
  `HSA_SNOOP_ROCM_VERSION`) to set the constant `host` and `rocm_version`
  labels, for container deployments.
- `--kernel-name-max` to cap the length of `kernel_name` label values.

### Changed
- The build-check CI jobs run against ROCm 7.14.0, 10.1.0 and 10.2.0 instead of
  10.1.0 only.

### Fixed
- `--mem-snoop` re-read `/proc/<pid>/maps` for every dispatch on the ring-poll
  thread. On a process with thousands of mappings this stalled the poller, so
  most launches were lost, one thread ran at 100% CPU regardless of `--poll-us`,
  and it kept running for about a minute after the workload stopped while it
  drained the ring. A snapshot of the map is now reused for up to one second.
- Kernel durations were computed from one timestamp per pass over all queues,
  so a slow pass on one queue appeared as a multi-second duration on another.
- Queues discovered by the kprobe were labelled with the creating thread's id
  and name; they now carry the process id and name, matching the KFD
  startup scan.
- `rocm_version` is detected on ROCm 10 layouts (`/opt/rocm/core/.info/version`).
- A kernel name that could not be resolved at first sight was cached as
  `kernel_0x<addr>` for the life of the process; the lookup is now retried a
  bounded number of times.
- No packets were decoded on ROCm 10.2 where the AQL ring cannot be read with
  `process_vm_readv`: device-memory rings, the new default on gfx94x and
  gfx125x with Large BAR, and gfx1201, where the cause is not yet known. Ring
  reads now fall back to `/proc/<pid>/mem`.
- A kprobe left enabled by an unclean exit is disabled before removal, instead
  of failing the next start with `File exists`.

## [1.6.0] — 2026-11-17

### Added
- `gen-prom-snapshot.py`: converts `prom/snapshot.json` to Prometheus exposition
  format so the full 109-series hardware-test snapshot can be imported into
  VictoriaMetrics during Grafana screenshot CI.

### Fixed
- Grafana screenshot CI now imports the full Prometheus snapshot from `gh-pages`,
  filling Overview gauges, Kernel Duration, Queue/Barrier, SDMA, and AIS panels
  that previously showed "No Data".
- `workflow_run` trigger in `grafana-screenshot.yml` updated to match the renamed
  workflow name after the `"hsa-snoop - "` prefix was dropped.
- Workflow `name:` fields stripped of `"hsa-snoop - "` prefix so `shields.io`
  derives badge labels correctly without `?label=` overrides.

## [1.5.0] — 2025-07-14

### Added
- GPU hardware timestamps via `HSA_TOOLS_LIB` shim and queue profiling support;
  `hsa_amd_queue_create` hook enables CP-side dispatch timestamps.
- AMD extended kernel dispatch packet decoding (vendor extension packets).
- Kernel completion detection via AQL completion signal value polling.
- Dispatch retirement when the completion signal fires.
- KFD debug-trap startup scan replaces the previous kfd-uprobe discovery mode.
- Kernel timing Grafana lane, docs page, and README performance shield.
- MkDocs Material GitHub Pages site replacing the previous static HTML site.

### Fixed
- `ksym` resolver now only probes mappings that can hold a code object.

### Changed
- CI builds against ROCm 10.1.

## [1.4.0] — 2025-05-19

### Added
- AIS write-direction hardware test and corrected perf badge labels.
- Grafana dashboard screenshot CI workflow.

### Changed
- Repository layout reorganised; Prometheus metrics cleaned up.
- README restructured; content migrated into `docs/` tree.

### Fixed
- Several Prometheus snapshot test reliability fixes (child-mode, `--all` mode,
  SDMA/AIS metric synthesis from JSON traces).

## [1.3.0] — 2025-04-07

### Added
- `kfd-uprobe` discovery mode for KFD/Linux emulated GPU support (WSL2 / APU).
- `--prometheus-snapshot` flag writes a point-in-time JSON snapshot for perf badges.
- Live observability stack (`docker compose`) with Loki, VictoriaMetrics, and
  an improved Grafana dashboard.
- Logrotate size cap and disk-full Grafana alerting rules.

### Fixed
- AIS monitor rewritten from bpftrace to tracefs kprobes for reliability.
- gfx1250/RDNA4 support; parser and ksym fixes; new hardware tests.
- Logrotate config installed to correct `sysconfdir` path in the Debian package.

## [1.2.0] — 2025-02-17

### Added
- SDMA v7 packet decoding for gfx12 (MI455X / gfx1250).
- GitHub Pages performance history infrastructure with per-run JSONL records.
- Hardware test CI workflow using the rocjitsu QEMU GPU emulator.
- Consolidated single systemd unit with new queue/XNACK Prometheus metrics.

### Fixed
- `pread64`/`off64_t` for pagemap access to handle high-VA SDMA ring offsets.
- Bounded all `sdma-test` invocations with a timeout; fixed v7 CMake regex.
- Missing `<algorithm>` include for `std::count` in `ais_monitor.cpp`.
- Debian package service lifecycle: correct upgrade vs. fresh-install paths.

## [1.1.1] — 2025-01-20

### Fixed
- Use `pread64`/`off64_t` for pagemap to fix high-VA offset overflow on
  processes with SDMA rings above 4 GB.

### Changed
- GitHub Actions dependencies bumped via Dependabot.

## [1.1.0] — 2025-01-06

### Added
- XNACK fault counter via kprobe on `kfd_process_vm_fault`.
- Memory footprint mode — kernarg buffer scanning via `/proc/<pid>/mem`.
- NDJSON dispatch log and offline dashboard replay harness.
- SDMA v6 packet decoding.
- Perfetto trace screenshots in documentation.

### Fixed
- Kernarg scan bounds and interior-pointer rejection.
- `/proc/<pid>/mem` fallback when `process_vm_readv` fails.
- SDMA broadcast-bit read restricted to LINEAR and TILED copy packets.
- Various slurm CI and observability fixes.

## [1.0.0] — 2024-12-09

Initial public release. Core features:

- Dynamic kprobe queue discovery; AQL compute and SDMA copy queue decoding.
- Perfetto protobuf trace output with queue-observed timing and kernel names.
- Prometheus exporter (`-DHSA_SNOOP_PROMETHEUS=ON`) with per-GPU metrics.
- AIS (AMD Infinity Storage) P2P direct-IO monitoring mode.
- Debian package with systemd service and logrotate config.
- Tag-triggered GitHub Release with `.deb` asset via `debian-publish` CI.

[Unreleased]: https://github.com/sbates130272/hsa-snoop/compare/v1.10.1...HEAD
[1.10.1]: https://github.com/sbates130272/hsa-snoop/compare/v1.10.0...v1.10.1
[1.10.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.9.0...v1.10.0
[1.9.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.8.0...v1.9.0
[1.8.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.7.0...v1.8.0
[1.7.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.6.0...v1.7.0
[1.6.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/sbates130272/hsa-snoop/releases/tag/v1.0.0
