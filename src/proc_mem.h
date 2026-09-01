// proc_mem.h - Cross-process memory reads (process_vm_readv) and physical
// address resolution (/proc/<pid>/pagemap) for a target process.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hsasnoop {

// Upper bound on how much of a kernarg block is ever scanned, and the ceiling
// used to sanity-check a kernel descriptor's declared kernarg size.
constexpr uint32_t kMaxKernargScanBytes = 4096;

// Read `len` bytes from virtual address `va` in process `pid` into `out`.
// Returns true on full read. Cheap enough to call in a tight poll loop.
bool ReadProcMem(int pid, uint64_t va, void* out, size_t len);

inline bool ReadU64(int pid, uint64_t va, uint64_t* out) {
    return ReadProcMem(pid, va, out, sizeof(*out));
}

// Read `len` bytes via /proc/<pid>/mem pread(). Slower than ReadProcMem but
// can access DRM-backed shared mappings that process_vm_readv cannot reach:
// the KFD maps both queue pointer pages (RDNA4 / gfx1201) and device
// allocations (CDNA3 / gfx942, CDNA4 / gfx950) inside /dev/dri/renderD* VMAs,
// and process_vm_readv refuses those with EFAULT.
bool ReadProcMemViaMem(int pid, uint64_t va, void* out, size_t len);

inline bool ReadU64ViaMem(int pid, uint64_t va, uint64_t* out) {
    return ReadProcMemViaMem(pid, va, out, sizeof(*out));
}

// Read the kernarg segment size from the AMDGPU kernel descriptor at
// `kernel_object` (the KD address carried in every AQL dispatch packet).
// Returns 0 when unavailable: the KD is unreadable, or the code object predates
// the v3 descriptor that introduced this field. Callers should fall back to a
// conservative fixed window in that case.
uint32_t ReadKernargSize(int pid, uint64_t kernel_object);

// Resolve the physical address backing user VA `va` in `pid` via pagemap.
// Returns 0 if not present/swapped or if the page is device memory (no PFN).
// Requires CAP_SYS_ADMIN (run under sudo).
uint64_t VirtToPhys(int pid, uint64_t va);

// True if the process is still alive.
bool ProcAlive(int pid);

// Scan the kernarg buffer at `kernarg_va` (size `kernarg_bytes`) in `pid` for
// 64-bit aligned values that are valid mapped addresses in the process's VM
// map. For each such pointer, accumulate the size of its backing /proc/maps
// region as an upper-bound VRAM footprint estimate.
//
// Returns false if the footprint could not be determined — the kernarg buffer
// was unreadable, or the process's VM map could not be read. Callers MUST
// check: on failure both out-params are zeroed, so a discarded return value
// turns "could not measure" into an indistinguishable and wrong "uses no
// memory". On success:
//   *mapped_vram_bytes  — sum of unique region sizes whose start address a
//                         candidate pointer falls within
//   *ptr_count          — number of candidate pointers found
//
// This is a best-effort heuristic: it will over-count if unrelated integer
// fields happen to match VA ranges, and under-count for pointer-chasing
// (nested pointers inside buffers). It requires no HSA runtime linkage.
bool ScanKernargFootprint(int pid, uint64_t kernarg_va, uint32_t kernarg_bytes,
                          uint64_t* mapped_vram_bytes, uint32_t* ptr_count);

} // namespace hsasnoop
