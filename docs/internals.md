# hsa-snoop internals

## How it works

hsa-snoop has three stages, all in user space:

1. **Queue discovery — via the amdgpu/KFD driver.**
   The ROCr runtime creates every AQL queue with the KFD `create_queue` ioctl
   (`kfd_ioctl_create_queue`). We install a **dynamic ftrace kprobe** on that
   function (by writing to `/sys/kernel/tracing/kprobe_events`) and read the
   `struct kfd_ioctl_create_queue_args` it was handed. That gives us, per queue:
   the ring base address, ring size, read/write pointers, GPU id and queue
   type. `COMPUTE_AQL` queues (type 2) are the host↔GPU AQL queues; `SDMA`
   (type 1) and `SDMA_XGMI` (type 3) are the DMA-copy queues. PM4 compute
   queues (type 0) are ignored.

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

## SDMA capture

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

> **Generation note.** KFD/ROCR publish SDMA ring pointers as byte offsets;
> hsa-snoop normalises them to dword indices before walking packets. It detects
> the SDMA generation from the KFD `gfx_target_version` property and uses
> separate packet tables for CDNA SDMA v4.x (gfx9), RDNA3 SDMA v6.x (gfx11),
> and gfx12 SDMA v7.x (gfx1250 / MI455X). Unknown generations are ignored
> rather than decoded with the wrong layout. Use `HSA_SNOOP_SDMA_DUMP=1` when
> calibrating a newly supported generation.

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

## Verifying the AQL layouts

`src/aql.h` is a self-contained copy of the packet formats so the tool builds
without ROCm `-dev` headers. It is byte-compatible with
`/opt/rocm/include/hsa/hsa.h`:
`static_assert`s enforce the 64-byte packet size, and field offsets match
`hsa_kernel_dispatch_packet_t`.
