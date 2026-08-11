#include "proc_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

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

// On some platforms (e.g. RDNA4/gfx1201) the write/read queue pointers live
// in /dev/dri/renderD128 DRM shared mappings that process_vm_readv refuses
// with EFAULT. /proc/<pid>/mem pread() goes through the VMA directly and
// succeeds on those pages. We keep a small fd cache so the hot poll path
// doesn't open/close on every call.
bool ReadU64ViaMem(int pid, uint64_t va, uint64_t* out) {
    // Simple thread-local fd cache: we're called from a single polling thread.
    static __thread int cached_pid = -1;
    static __thread int cached_fd = -1;

    if (cached_pid != pid) {
        if (cached_fd >= 0)
            close(cached_fd);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        cached_fd = open(path, O_RDONLY);
        cached_pid = pid;
    }
    if (cached_fd < 0)
        return false;
    return pread(cached_fd, out, sizeof(*out), static_cast<off_t>(va)) ==
           static_cast<ssize_t>(sizeof(*out));
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

namespace {

struct VmaRegion {
    uint64_t start;
    uint64_t end;
};

// Read /proc/<pid>/maps and return all readable, non-zero-length regions.
// Caller can then binary-search for a candidate VA.
std::vector<VmaRegion> ReadVmas(int pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::ifstream f(path);
    std::vector<VmaRegion> vmas;
    std::string line;
    while (std::getline(f, line)) {
        uint64_t start = 0, end = 0;
        if (sscanf(line.c_str(), "%lx-%lx", &start, &end) == 2 && end > start)
            vmas.push_back({start, end});
    }
    return vmas;
}

// Return the region whose [start, end) contains `addr`, or nullptr.
const VmaRegion* FindVma(const std::vector<VmaRegion>& vmas, uint64_t addr) {
    // vmas from /proc/<pid>/maps arrive in ascending order.
    for (const auto& v : vmas) {
        if (addr >= v.start && addr < v.end)
            return &v;
        if (v.start > addr)
            break;
    }
    return nullptr;
}

} // namespace

bool ScanKernargFootprint(int pid, uint64_t kernarg_va, uint32_t kernarg_bytes,
                          uint64_t* mapped_vram_bytes, uint32_t* ptr_count) {
    *mapped_vram_bytes = 0;
    *ptr_count = 0;

    if (!kernarg_va || kernarg_bytes < 8)
        return true; // nothing to scan; not an error

    // Cap at 4 KB to bound cost; real kernarg blocks are almost always < 256 B.
    const uint32_t kMaxScan = 4096;
    uint32_t scan_bytes = (kernarg_bytes < kMaxScan) ? kernarg_bytes : kMaxScan;
    // Align down to 8.
    scan_bytes &= ~7u;
    if (scan_bytes < 8)
        return true;

    std::vector<uint8_t> buf(scan_bytes);
    if (!ReadProcMem(pid, kernarg_va, buf.data(), scan_bytes))
        return false;

    std::vector<VmaRegion> vmas = ReadVmas(pid);
    if (vmas.empty())
        return false;

    // Track which region starts we've already counted to avoid double-counting
    // two pointers into the same allocation.
    std::unordered_set<uint64_t> seen_starts;

    for (uint32_t off = 0; off + 8 <= scan_bytes; off += 8) {
        uint64_t candidate = 0;
        memcpy(&candidate, buf.data() + off, sizeof(candidate));

        // Heuristic filter: user-space GPU VAs on Linux/amdgpu are typically
        // in the range [4 GB, 256 TB]. Skip nulls, small integers, and
        // addresses that cannot plausibly be GPU BOs.
        if (candidate < (1ULL << 32) || candidate >= (1ULL << 48))
            continue;

        const VmaRegion* v = FindVma(vmas, candidate);
        if (!v)
            continue;

        (*ptr_count)++;
        if (seen_starts.insert(v->start).second)
            *mapped_vram_bytes += (v->end - v->start);
    }
    return true;
}

} // namespace hsasnoop
