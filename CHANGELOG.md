# Changelog

All notable changes to hsa-snoop are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/sbates130272/hsa-snoop/compare/v1.7.0...HEAD
[1.7.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.6.0...v1.7.0
[1.6.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.5.0...v1.6.0
[1.5.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/sbates130272/hsa-snoop/releases/tag/v1.0.0
