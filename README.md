# hsa-snoop: A HIP/HSA-Agnostic AMD GPU Runtime Analyzer

[![vm-hardware-test](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-vm-hardware-test.yml/badge.svg?branch=main)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-vm-hardware-test.yml)
[![build-check](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-build-check.yml/badge.svg)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-build-check.yml)
[![clang-format](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-clang-format.yml/badge.svg)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-clang-format.yml)
[![spell-check](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-spell-check.yml/badge.svg)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-spell-check.yml)
[![debian-package](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-debian-package.yml/badge.svg)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-debian-package.yml)
[![debian-publish](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-debian-publish.yml/badge.svg?branch=main)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-debian-publish.yml)
[![docs-deploy](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-docs-deploy.yml/badge.svg?branch=main)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-docs-deploy.yml)
[![kernel launches](https://img.shields.io/endpoint?url=https://sbates130272.github.io/hsa-snoop/perf/badge-kernel_launches.json&cacheSeconds=3600)](https://sbates130272.github.io/hsa-snoop/perf/)
[![SDMA copies](https://img.shields.io/endpoint?url=https://sbates130272.github.io/hsa-snoop/perf/badge-sdma_copies.json&cacheSeconds=3600)](https://sbates130272.github.io/hsa-snoop/perf/)
[![AIS RX bytes](https://img.shields.io/endpoint?url=https://sbates130272.github.io/hsa-snoop/perf/badge-ais_rx_bytes.json&cacheSeconds=3600)](https://sbates130272.github.io/hsa-snoop/perf/)
[![AIS TX bytes](https://img.shields.io/endpoint?url=https://sbates130272.github.io/hsa-snoop/perf/badge-ais_tx_bytes.json&cacheSeconds=3600)](https://sbates130272.github.io/hsa-snoop/perf/)
[![grafana-screenshot](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-grafana-screenshot.yml/badge.svg?branch=main)](https://github.com/sbates130272/hsa-snoop/actions/workflows/hsa-snoop-grafana-screenshot.yml)
[![Grafana Dashboard](https://img.shields.io/badge/Grafana-Dashboard-F46800?logo=grafana)](https://github.com/sbates130272/hsa-snoop/blob/main/deploy/grafana/hsa-snoop-dashboard.json)
[![Perfetto Tracing](https://img.shields.io/badge/Perfetto-Traces-blueviolet)](https://ui.perfetto.dev)

`hsa-snoop` traces AMD GPU workloads from user space — without touching the
driver. It discovers AQL compute queues and SDMA copy queues via a dynamic
kprobe, decodes all packet types including kernel dispatches and DMA copies,
and emits Perfetto traces with queue-observed timing, kernel names, byte counts
and directions.

```
sudo ./hsa-snoop -- ./my_hip_app     # launch + trace a single app
sudo ./hsa-snoop --all               # monitor every GPU process system-wide
# open traces at https://ui.perfetto.dev
```

Project pages: [GitHub Pages](https://sbates130272.github.io/hsa-snoop/) ·
[Performance tracker](https://sbates130272.github.io/hsa-snoop/perf/) ·
[Prometheus snapshot](https://sbates130272.github.io/hsa-snoop/prom/)

→ [How it works in detail](docs/internals.md)

## Perfetto Trace Visualization

Each AQL or SDMA queue gets its own timeline track; selecting a slice exposes
decoded metadata including grid dimensions, kernel object, segment sizes and
completion signals. Output is native Perfetto protobuf (`.pftrace`) or Chrome
JSON (`--format json`); both open in <https://ui.perfetto.dev>.

→ [Trace visualization screenshots](docs/perfetto.md)

---

## Building

```
cmake -B build
cmake --build build --parallel $(nproc)
```

Requires a C++17 compiler and CMake 3.21+. No third-party libraries. `protoc`
is optional. If ROCm is installed, the HIP examples under `deploy/examples/`
are built automatically; pass `-DGPU_TARGETS=gfxNNNN` to override detection.

### Testing

```
cmake -B build -DBUILD_TESTING=ON
cmake --build build --parallel $(nproc)
ctest --test-dir build --output-on-failure
```

Hardware tests (require a GPU and root):

```
cmake -B build-hw -DBUILD_TESTING=ON -DHSA_SNOOP_HARDWARE_TESTS=ON
cmake --build build-hw --parallel $(nproc)
sudo ctest --test-dir build-hw -L hardware --output-on-failure
```

### Building with Prometheus Exporter Support

```
cmake -B build -DHSA_SNOOP_PROMETHEUS=ON
cmake --build build --parallel $(nproc)
```

Fetches [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp) v1.2.4 via
`FetchContent`; no other dependencies required.

---

## Usage

Requires root (tracefs, pagemap, and cross-process reads all need it).

```
sudo ./hsa-snoop --help
```

→ [Full usage guide: all modes, systemd setup, dispatch log](docs/usage.md)

---

## Prometheus Exporter

Optional — build with `-DHSA_SNOOP_PROMETHEUS=ON`. Exposes live GPU metrics
at `http://localhost:9488/metrics`.

→ [Prometheus metrics reference](docs/prometheus.md)

---

## Grafana Dashboard

→ [Grafana dashboard and development workflow](docs/grafana.md)

---

## Further Reading

- [Perfetto Trace Visualization](docs/perfetto.md)
- [How It Works, SDMA Capture, Driver Details](docs/internals.md)
- [Usage Guide: All Modes, systemd, Dispatch Log](docs/usage.md)
- [Prometheus Exporter Metrics Reference](docs/prometheus.md)
- [Grafana Dashboard and Development Workflow](docs/grafana.md)
- [Project Layout](docs/layout.md)
- [Limitations and Notes](docs/limitations.md)
