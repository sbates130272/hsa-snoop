#include "proc_mem.h"

#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>

namespace hsasnoop {

bool ReadProcMem(int pid, uint64_t va, void* out, size_t len) {
    struct iovec local {
        out, len
    };
    struct iovec remote {
        reinterpret_cast<void*>(va), len
    };
    ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    return n == static_cast<ssize_t>(len);
}

uint64_t VirtToPhys(int pid, uint64_t va) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    const uint64_t page = sysconf(_SC_PAGESIZE);
    uint64_t entry = 0;
    off_t off = (va / page) * sizeof(uint64_t);
    uint64_t phys = 0;
    if (pread(fd, &entry, sizeof(entry), off) == sizeof(entry)) {
        const uint64_t kPresent = 1ULL << 63;
        const uint64_t kPfnMask = (1ULL << 55) - 1;
        if (entry & kPresent) {
            uint64_t pfn = entry & kPfnMask;
            if (pfn)
                phys = pfn * page + (va % page);
        }
    }
    close(fd);
    return phys;
}

bool ProcAlive(int pid) {
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d", pid);
    return access(path, F_OK) == 0;
}

} // namespace hsasnoop
