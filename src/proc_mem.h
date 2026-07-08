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

// Resolve the physical address backing user VA `va` in `pid` via pagemap.
// Returns 0 if not present/swapped or if the page is device memory (no PFN).
// Requires CAP_SYS_ADMIN (run under sudo).
uint64_t VirtToPhys(int pid, uint64_t va);

// True if the process is still alive.
bool ProcAlive(int pid);

} // namespace hsasnoop
