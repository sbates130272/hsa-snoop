# hsa-snoop

`hsa-snoop` detects the HSA **AQL queues** an application uses to talk to an AMD
GPU, tracks their ring-buffer addresses (virtual **and** physical), decodes the
packets flowing across them (kernel dispatches, barriers, agent dispatches), and
emits a **Perfetto** trace with per-dispatch timing, kernel names, launch
dimensions and more.

It works **without modifying or rebuilding the amdgpu driver**. See
[How it works](#how-it-works) and [Do we need to touch the
driver?](#do-we-need-to-touch-the-driver) below.

```
sudo ./hsa-snoop -- ./my_hip_app          # launch + trace, writes hsa-snoop.pftrace
# open the trace at https://ui.perfetto.dev
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
   the ring base address, ring size, read/write dispatch-id pointers, GPU id and
   queue type. Only `COMPUTE_AQL` queues (type 2) are host↔GPU AQL queues; the
   rest (PM4/SDMA) are ignored.

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
   * when the read id passes a packet, it is marked complete.

   Submit → complete host timestamps (`CLOCK_MONOTONIC_RAW`) give an approximate
   on-GPU window for each dispatch.

   **Kernel names** are resolved from `kernel_object` (the runtime address of the
   kernel descriptor, i.e. the ELF `*.kd` symbol). We locate the AMDGPU
   (`EM_AMDGPU`) code-object ELFs in the process address space and walk their
   dynamic symbol tables. The descriptors the GPU actually uses live in
   device-backed (`/dev/dri/renderD128`) mappings, which `process_vm_readv`
   cannot read, so the resolver reads code objects through `/proc/<pid>/mem`
   instead. Names are demangled.

Output is written as a native Perfetto protobuf trace (default) or Chrome JSON
(`--format json`); both open in <https://ui.perfetto.dev>.

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

## Usage

```
sudo ./hsa-snoop [options] --pid <pid>       # attach to a running process
sudo ./hsa-snoop [options] -- <command...>   # launch and trace a command

Options:
  --pid <pid>        Trace only this process
  --out <file>       Output path (default hsa-snoop.pftrace / .json)
  --format <fmt>     perfetto (default) | json
  --poll-us <n>      Ring poll interval in microseconds (default 20)
  --duration <sec>   Auto-stop after N seconds (0 = until Ctrl-C / child exit)
  --tracefs <path>   tracefs mount (default /sys/kernel/tracing)
```

Must run as root (tracefs, pagemap and cross-process reads all require it).

### Recommended: launch mode

```
sudo ./hsa-snoop -- ./my_hip_app
```

The kprobe is armed *before* the child is exec'd, so every queue it creates is
captured. Set `HSA_SNOOP_DEBUG=1` for verbose discovery/decode diagnostics.

### Quick discovery-only check (no build)

```
sudo bpftrace scripts/hsa-snoop.bt
```

## Layout

```
src/aql.{h,cpp}          AQL packet formats (mirrors <hsa/hsa.h>)
src/model.h              QueueInfo / PacketRecord data model
src/discovery.{h,cpp}    ftrace-kprobe queue discovery
src/proc_mem.{h,cpp}     process_vm_readv + pagemap (VA->phys)
src/ksym.{h,cpp}         kernel_object -> demangled name via code-object ELFs
src/parser.{h,cpp}       per-queue ring poller / AQL decoder / timing
src/trace_writer.{h,cpp} Perfetto protobuf + Chrome JSON writers
src/main.cpp             CLI / orchestration
scripts/hsa-snoop.bt     zero-build discovery via bpftrace
```

## Limitations & notes

* **Sampling snoop, not a tap.** Activity is captured by polling the ring. A
  kernel that is enqueued *and* completes entirely within one poll interval can
  be missed (the slot may read back as `INVALID` once consumed). Lower
  `--poll-us` to catch faster kernels; deeply-queued workloads are captured
  near-completely (195/200 in the bundled test). Missed packets are counted
  internally as "dropped".
* **Timing is host-observed.** submit→complete uses host timestamps for
  enqueue/dequeue transitions, so a slice includes queue-wait time and is an
  approximation of the true on-GPU execution window (no profiling
  signals/counters are read). Good for structure and relative timing.
* **Attach mode only sees queues created after attach**, since the kprobe fires
  on `create_queue`. Use launch mode to guarantee capture from process start.
* **Physical addresses** resolve for host/GTT-backed rings (the norm for AQL
  host↔GPU queues). VRAM-resident pages have no pagemap PFN and report `0`.
* Verified on: gfx90a (MI2xx / Aldebaran), ROCm 7.1.0, amdgpu 6.16 DKMS,
  kernel 6.8, x86-64.

## Verifying the AQL layouts

`src/aql.h` is a self-contained copy of the packet formats so the tool builds
without ROCm `-dev` headers. It is byte-compatible with
`/opt/rocm/include/hsa/hsa.h`:
`static_assert`s enforce the 64-byte packet size, and field offsets match
`hsa_kernel_dispatch_packet_t`.
