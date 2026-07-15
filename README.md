# hsa-snoop

`hsa-snoop` detects the HSA **AQL queues** and **SDMA copy queues** an
application uses to talk to an AMD GPU, tracks their ring-buffer addresses
(virtual **and** physical), decodes the packets flowing across them (kernel
dispatches, barriers, agent dispatches, and SDMA DMA copies), and emits a
**Perfetto** trace with queue-observed timing, kernel names, launch dimensions,
copy byte counts and directions, and more.

SDMA queues carry the DMA-engine traffic — `hipMemcpyAsync`, peer-to-peer
copies, KV-cache staging — that never appears on the AQL compute queues, so
capturing them is the only way to see bulk device I/O byte counts from user
space. See [SDMA capture](#sdma-capture).

It works **without modifying or rebuilding the amdgpu driver**. See
[How it works](#how-it-works) and [Do we need to touch the
driver?](#do-we-need-to-touch-the-driver) below.

```
sudo ./hsa-snoop -- ./my_hip_app     # launch + trace a single app
sudo ./hsa-snoop --all               # monitor every GPU process system-wide
# open traces at https://ui.perfetto.dev
```

Example (from the bundled `deep` micro-benchmark):

```
[queue] pid=404039 comm=deep uid=2 ring_va=0x75dbfc200000 ring_phys=0x1a781498000 size=1048576B slots=16384 gpu=36740
hsa-snoop: 2 AQL queue(s), 195 packet(s) captured.
```

...producing slices named `vadd(float*, float*, float*, int)` /
`vscale(float*, float, int)` on a per-queue track, each annotated with grid,
workgroup, `kernel_object`, group/private segment sizes, completion signal and
the ring's physical address.

---

## How it works

hsa-snoop has three stages, all in user space:

1. **Queue discovery — via the amdgpu/KFD driver.**
   The ROCr runtime creates every AQL queue with the KFD `create_queue` ioctl
   (`kfd_ioctl_create_queue`). We install a **dynamic ftrace kprobe** on that
   function (by writing to `/sys/kernel/tracing/kprobe_events`) and read the
   `struct kfd_ioctl_create_queue_args` it was handed. That gives us, per queue:
   the ring base address, ring size, read/write pointers, GPU id and queue
   type. `COMPUTE_AQL` queues (type 2) are the host↔GPU AQL queues; `SDMA`
   (type 1) and `SDMA_XGMI` (type 3) are the DMA-copy queues (see
   [SDMA capture](#sdma-capture)). PM4 compute queues (type 0) are ignored.

   The argument offsets come from the stable KFD uapi ABI
   (`struct kfd_ioctl_create_queue_args`), and the args pointer is the 3rd
   function argument (`%dx` on x86-64). No BTF or driver debuginfo is required.

   A zero-build equivalent is provided for quick verification:
   `sudo bpftrace scripts/hsa-snoop.bt`.

2. **Physical address tracking.**
   AQL ring buffers for host↔GPU communication live in host-accessible
   (GTT/system) memory. For each ring we resolve its physical address by walking
   `/proc/<pid>/pagemap` (needs `CAP_SYS_ADMIN`, i.e. run under `sudo`).

3. **Ring parsing — the activity snoop.**
   The ring base and read/write pointers captured in step 1 are **user virtual
   addresses in the target process** (the KFD validates them as `__user`
   pointers). A per-queue poller reads them with `process_vm_readv` and watches
   `write_dispatch_id` vs `read_dispatch_id`:
   * when the write id advances, the newly enqueued 64-byte AQL packet is decoded
     using the layouts in `src/aql.h` (mirrors `<hsa/hsa.h>`);
   * when the read id passes a packet, it is marked queue-consumed.

   Enqueue → queue-consumed host timestamps (`CLOCK_MONOTONIC_RAW`) give an
   approximate queue residency window for each dispatch. This is not the same as
   observing the packet completion signal or true GPU execution duration.

   **Kernel names** are resolved from `kernel_object` (the runtime address of the
   kernel descriptor, i.e. the ELF `*.kd` symbol). We locate the AMDGPU
   (`EM_AMDGPU`) code-object ELFs in the process address space and walk their
   dynamic symbol tables. The descriptors the GPU actually uses live in
   device-backed (`/dev/dri/renderD128`) mappings, which `process_vm_readv`
   cannot read, so the resolver reads code objects through `/proc/<pid>/mem`
   instead. Names are demangled.

Output is written as a native Perfetto protobuf trace (default) or Chrome JSON
(`--format json`); both open in <https://ui.perfetto.dev>.

### SDMA capture

The same discovery kprobe also sees the **SDMA** (`type 1`) and **SDMA_XGMI**
(`type 3`) queues — the GPU DMA copy engines. `hipMemcpyAsync`, peer-to-peer
transfers and framework KV-cache staging are submitted here, **not** on the AQL
compute queues, so they are invisible to `hsa_kernel_launches_total`. Capturing
SDMA is the only user-space way to account for that bulk byte movement.

SDMA rings differ from AQL rings in three ways the parser handles separately
(`src/sdma.h`, `src/parser.cpp`):

* **Variable-length packets.** SDMA is dword-granular microcode, not fixed
  64-byte slots. Each packet's header dword carries an 8-bit opcode + sub-opcode;
  the decoder reads the header, computes the length, decodes, then strides
  forward. Unknown opcodes trigger a resync rather than a bad stride.
* **The COPY packet carries the byte count and src/dst addresses.** For linear
  copies we record `bytes`, `src`, `dst` and a **direction**.
* **Direction (H2D / D2H / D2D)** is inferred by resolving the src/dst virtual
  addresses through `/proc/<pid>/pagemap`: a host page has a physical frame, a
  device (VRAM) page has none. src=host,dst=device → `h2d`, and so on. Results
  are cached per page.

> **Calibration note.** SDMA ring pointer units (dword vs byte) and the
> linear-copy `COUNT` field width are hardware-revision sensitive. The defaults
> target the CDNA SDMA v4.x family (gfx90a / gfx942, MI2xx/MI3xx). Confirm them
> on a new platform by running with `HSA_SNOOP_DEBUG=1` and checking the ring
> dump strides against the `wptr` deltas (`kSdmaPtrIsDwords` /
> `kLinearCopyCountMask` in `src/parser.cpp` / `src/sdma.h`).

## Do we need to touch the driver?

**No.** Everything is done by dynamically attaching to the running,
stock out-of-tree amdgpu module:

* queue discovery — dynamic **ftrace kprobe** on `kfd_ioctl_create_queue`
  (or the bundled bpftrace script). No source patch, no rebuild, no reload.
* physical addresses — `/proc/<pid>/pagemap`.
* ring contents & pointers — `process_vm_readv` / `/proc/<pid>/mem`.
* kernel names — the process's own loaded code objects.

The KFD debugfs nodes (`/sys/kernel/debug/kfd/mqds`, `hqds`, `rls`) can be used
as an independent cross-check of ring addresses but are not required.

A source-level tracepoint patch to amdgpu would only ever be needed if dynamic
kprobe attach were unavailable (e.g. `kfd_ioctl_create_queue` inlined away); it
is **not** needed on this platform (gfx90a, amdgpu 6.16 DKMS).

## Build

```
cmake -B build
cmake --build build --parallel $(nproc)
```

Requires a C++17 compiler and CMake 3.21+. No third-party libraries. `protoc`
is optional (only used to sanity-check the emitted trace).

If ROCm is installed, the HIP examples under `examples/` are built
automatically. GPU targets are auto-detected via `rocm_agent_enumerator`; pass
`-DGPU_TARGETS=gfxNNNN` to override.

### Prometheus exporter build

To enable the Prometheus metrics exporter, pass `-DHSA_SNOOP_PROMETHEUS=ON`.
This fetches [prometheus-cpp](https://github.com/jupp0r/prometheus-cpp)
(v1.2.4) via CMake `FetchContent` and links it into the binary:

```
cmake -B build -DHSA_SNOOP_PROMETHEUS=ON
cmake --build build --parallel $(nproc)
```

No other dependencies are required; the prometheus-cpp HTTP server
(CivetWeb) is bundled.

## Usage

```
sudo ./hsa-snoop [options] --pid <pid>       # attach to a running process
sudo ./hsa-snoop [options] -- <command...>   # launch and trace a command
sudo ./hsa-snoop [options] --all             # monitor all GPU processes

Options:
  --pid <pid>        Trace only this process
  --all              Monitor all HSA/HIP processes system-wide (daemon mode);
                     writes one trace file per discovered queue to --out-dir
  --out <file>       Output trace path (single-process modes)
                     (default hsa-snoop.pftrace / .json)
  --out-dir <dir>    Output directory for --all mode
                     (default /var/log/hsa-snoop)
  --format <fmt>     perfetto (default) | json
  --poll-us <n>      Ring poll interval in microseconds (default 20)
  --duration <sec>   Auto-stop after N seconds (0 = until Ctrl-C / child exit)
  --tracefs <path>   tracefs mount (default /sys/kernel/tracing)
  --mode <mode>      kprobe (default) | bpftrace
                     bpftrace: for WSL2/librocdxg systems without KFD;
                     auto-selected when running inside WSL2
  --librocdxg <path> Path to librocdxg.so.* (bpftrace mode; auto-detected)
  --prometheus       Expose metrics via Prometheus HTTP endpoint instead of
                     writing trace files (requires -DHSA_SNOOP_PROMETHEUS=ON)
  --prometheus-port <n>
                     Port for the Prometheus /metrics endpoint (default 9488)
```

Must run as root (tracefs, pagemap and cross-process reads all require it).

### Recommended: launch mode

```
sudo ./hsa-snoop -- ./my_hip_app
```

The kprobe is armed *before* the child is exec'd, so every queue it creates is
captured. Set `HSA_SNOOP_DEBUG=1` for verbose discovery/decode diagnostics.

### System-wide monitor mode

```
sudo ./hsa-snoop --all
```

Monitors every HSA/HIP process on the system without targeting a specific PID.
Each discovered AQL queue writes its own trace file to `/var/log/hsa-snoop/`
(override with `--out-dir`). Runs indefinitely until `Ctrl-C` or `SIGTERM`.
This is the mode used by the systemd daemon.

### WSL2 / librocdxg mode

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

### Prometheus exporter mode

When built with `-DHSA_SNOOP_PROMETHEUS=ON`, hsa-snoop can expose live GPU
metrics via a Prometheus-compatible HTTP endpoint instead of writing trace
files. This integrates with Grafana and any Prometheus-compatible monitoring
stack.

```
sudo ./hsa-snoop --all --prometheus                  # serve on default port 9488
sudo ./hsa-snoop --all --prometheus-port 9100        # custom port
sudo ./hsa-snoop --pid <pid> --prometheus            # single process
```

Scrape the metrics endpoint:

```
curl http://localhost:9488/metrics
```

Metrics exposed (all carry constant labels `host`, `platform`, `rocm_version`,
`discovery_mode`):

| Metric | Type | Labels | Description |
| --- | --- | --- | --- |
| `hsa_snoop_up` | Gauge | — | 1 when the exporter is running |
| `hsa_kernel_launches_total` | Counter | `kernel_name`, `gpu_id`, `gpu_type`, `pid`, `comm` | Kernel dispatch count; filter by `kernel_name` for per-kernel totals |
| `hsa_kernel_duration_seconds` | Histogram | `kernel_name`, `gpu_id`, `gpu_type` | Kernel duration from enqueue to completion (host-observed) |
| `hsa_kernel_launches_per_second` | Gauge | `gpu_id`, `gpu_type` | Rolling 10 s launch rate |
| `hsa_last_kernel_launch_timestamp_seconds` | Gauge | `gpu_id`, `gpu_type` | Wall-clock time of the most recent launch |
| `hsa_last_kernel_launch_info` | Gauge | `gpu_id`, `gpu_type`, `kernel_name` | Most recently launched kernel name (value always 1) |
| `hsa_active_queues` | Gauge | `gpu_id`, `gpu_type` | Currently tracked AQL queues |
| `hsa_barrier_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm` | Barrier AQL packets seen |
| `hsa_sdma_copies_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | SDMA copy packets, by direction (`h2d`/`d2h`/`d2d`) |
| `hsa_sdma_bytes_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `direction` | Bytes moved by SDMA copies, by direction |
| `hsa_sdma_packets_total` | Counter | `gpu_id`, `gpu_type`, `pid`, `comm`, `opcode` | SDMA packets, by opcode (`copy`/`fence`/`timestamp`/...) |
| `hsa_active_sdma_queues` | Gauge | `gpu_id`, `gpu_type` | Currently tracked SDMA queues |
| `hsa_errors_total` | Counter | `gpu_id`, `pid` | Packets arriving before queue registration |

Prometheus mode and trace-file mode are **independent** — both can run
simultaneously using separate hsa-snoop instances (or via the two systemd
units below). They do not share state or interfere with each other.

### Quick discovery-only check (no build)

```
sudo bpftrace scripts/hsa-snoop.bt
```

## Running as a systemd daemon

Two independent unit files are provided under `systemd/`:

| Unit | Mode | Output |
| --- | --- | --- |
| `hsa-snoop.service` | `--all` (trace files) | `/var/log/hsa-snoop/*.pftrace` |
| `hsa-snoop-prometheus.service` | `--all --prometheus` | HTTP `/metrics` on port 9488 |

Both can run simultaneously — they use separate discovery probes and do not
share state.

### Trace-file daemon

```bash
# Install binary and unit files
sudo cmake --install build --prefix /usr/local

# (Optional) drop-in to pick up /etc/hsa-snoop.conf for runtime tuning
sudo mkdir -p /etc/systemd/system/hsa-snoop.service.d
sudo cp systemd/hsa-snoop-override.conf \
        /etc/systemd/system/hsa-snoop.service.d/override.conf
sudo cp systemd/hsa-snoop.conf /etc/hsa-snoop.conf

# Create the log directory
sudo mkdir -p /var/log/hsa-snoop

# Enable and start
sudo systemctl daemon-reload
sudo systemctl enable --now hsa-snoop

# Check status / live logs
sudo systemctl status hsa-snoop
sudo journalctl -fu hsa-snoop
```

Traces accumulate in `/var/log/hsa-snoop/` as `.pftrace` files, one per
discovered AQL queue, named `<comm>-<uid>.pftrace`. Open them in
<https://ui.perfetto.dev>.

To tune the daemon (poll interval, output format, etc.) edit
`/etc/hsa-snoop.conf` and run `sudo systemctl restart hsa-snoop`.

Filesystem usage is managed by **logrotate** — trace files are rotated at
512 MiB and the last 8 rotated copies are kept (compressed), giving a soft
ceiling of ~4 GiB. Triggered daily by the system logrotate timer. Configuration
is installed to `/etc/logrotate.d/hsa-snoop`; edit it to adjust size, count,
or compression.

### Prometheus daemon

Requires a build with `-DHSA_SNOOP_PROMETHEUS=ON`.

```bash
# Install binary and unit files (if not already done above)
sudo cmake --install build --prefix /usr/local

# Edit the unit to set SUDO_UID/SUDO_GID to the primary GPU-capable user
# (needed for gpu_type detection via rocminfo on WSL2/DXG systems).
# The defaults in the unit file are UID/GID 1000.
sudo systemctl daemon-reload
sudo systemctl enable --now hsa-snoop-prometheus

# Check status / live logs
sudo systemctl status hsa-snoop-prometheus
sudo journalctl -fu hsa-snoop-prometheus

# Verify the metrics endpoint
curl http://localhost:9488/metrics
```

To change the port, override `ExecStart` in a drop-in:

```bash
sudo mkdir -p /etc/systemd/system/hsa-snoop-prometheus.service.d
cat <<EOF | sudo tee /etc/systemd/system/hsa-snoop-prometheus.service.d/port.conf
[Service]
ExecStart=
ExecStart=/usr/local/bin/hsa-snoop --all --prometheus --prometheus-port 9100
EOF
sudo systemctl daemon-reload && sudo systemctl restart hsa-snoop-prometheus
```

## Layout

```
src/aql.{h,cpp}                    AQL packet formats (mirrors <hsa/hsa.h>)
src/sdma.{h,cpp}                   SDMA microcode packet formats + length decode
src/model.h                        QueueInfo / PacketRecord / SdmaRecord data model
src/discovery.{h,cpp}              queue discovery: kprobe (native) or bpftrace (WSL2)
src/proc_mem.{h,cpp}               process_vm_readv + pagemap (VA->phys)
src/ksym.{h,cpp}                   kernel_object -> demangled name via code-object ELFs
src/parser.{h,cpp}                 per-queue ring poller / AQL + SDMA decoder / timing
src/trace_writer.{h,cpp}           Perfetto protobuf + Chrome JSON writers
src/prometheus_exporter.{h,cpp}    Prometheus metrics exporter (optional)
src/main.cpp                       CLI / orchestration
scripts/hsa-snoop.bt               zero-build discovery via bpftrace
systemd/hsa-snoop.service          systemd unit (trace-file daemon / --all mode)
systemd/hsa-snoop-prometheus.service  systemd unit (Prometheus exporter daemon)
systemd/hsa-snoop.conf             runtime configuration (ExecStart arguments)
systemd/hsa-snoop-override.conf    drop-in wiring hsa-snoop.conf into the unit
systemd/hsa-snoop.logrotate        logrotate policy (512 MiB rotation, 8 kept)
```

## Limitations & notes

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
* **WSL2 PID mapping is heuristic.** bpftrace reports host-kernel PIDs, which
  differ from WSL2 namespace PIDs by a fixed offset. In `--all` mode without a
  known target PID, hsa-snoop scans `/proc` by comm name to resolve the
  namespace PID. This can misattribute queues if two processes share the same
  comm prefix. Launch mode (`-- <command>`) avoids this by using the known
  child PID.
* **Physical addresses** resolve for host/GTT-backed rings (the norm for AQL
  host↔GPU queues). VRAM-resident pages have no pagemap PFN and report `0`.
* **SDMA decode is calibrated for CDNA SDMA v4.x** (gfx90a / gfx942). The ring
  pointer unit and linear-copy `COUNT` width are hardware-revision sensitive; on
  a new platform confirm them with `HSA_SNOOP_DEBUG=1` (see
  [SDMA capture](#sdma-capture)). Non-linear copy layouts (tiled, sub-window) are
  counted and strided over but do not yet contribute a byte total.
* **SDMA copy direction is a pagemap heuristic.** A src/dst page with no PFN is
  treated as device (VRAM) memory; this also matches a not-present/swapped host
  page, so a copy touching a paged-out host buffer can be misclassified. For
  live, pinned copy buffers (the common case) it is accurate.
* Verified on: gfx90a (MI2xx / Aldebaran), ROCm 7.1.0, amdgpu 6.16 DKMS,
  kernel 6.8, x86-64. SDMA capture calibration in progress on gfx942 (MI3xx).

## Verifying the AQL layouts

`src/aql.h` is a self-contained copy of the packet formats so the tool builds
without ROCm `-dev` headers. It is byte-compatible with
`/opt/rocm/include/hsa/hsa.h`:
`static_assert`s enforce the 64-byte packet size, and field offsets match
`hsa_kernel_dispatch_packet_t`.
