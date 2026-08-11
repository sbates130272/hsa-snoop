// proc_mem.h - Cross-process memory reads (process_vm_readv) and physical
// address resolution (/proc/<pid>/pagemap) for a target process.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hsasnoop {

// Read `len` bytes from virtual address `va` in process `pid` into `out`.
// Returns true on full read. Cheap enough to call in a tight poll loop.
bool ReadProcMem(int pid, uint64_t va, void* out, size_t len);

inline bool ReadU64(int pid, uint64_t va, uint64_t* out) {
    return ReadProcMem(pid, va, out, sizeof(*out));
}

// Read a u64 via /proc/<pid>/mem pread(). Slower than ReadProcMem but can
// access DRM-backed shared mappings that process_vm_readv cannot reach (e.g.
// the write/read pointer pages on RDNA4 / gfx1201 which live in
// /dev/dri/renderD128 mmap'd regions). Use for pointer addresses only.
bool ReadU64ViaMem(int pid, uint64_t va, uint64_t* out);

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
// Returns false if the kernarg buffer cannot be read. On success:
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
