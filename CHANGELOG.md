# Changelog

All notable changes to hsa-snoop are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] — v1.6.0

### Added
- `gen-prom-snapshot.py`: converts `prom/snapshot.json` to Prometheus exposition
  format so the full 109-series hardware-test snapshot can be imported into
  VictoriaMetrics during Grafana screenshot CI.

### Fixed
- Grafana screenshot CI now imports the full Prometheus snapshot from gh-pages,
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

[Unreleased]: https://github.com/sbates130272/hsa-snoop/compare/v1.5.0...HEAD
[1.5.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.4.0...v1.5.0
[1.4.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.3.0...v1.4.0
[1.3.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.2.0...v1.3.0
[1.2.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.1...v1.2.0
[1.1.1]: https://github.com/sbates130272/hsa-snoop/compare/v1.1.0...v1.1.1
[1.1.0]: https://github.com/sbates130272/hsa-snoop/compare/v1.0.0...v1.1.0
[1.0.0]: https://github.com/sbates130272/hsa-snoop/releases/tag/v1.0.0
