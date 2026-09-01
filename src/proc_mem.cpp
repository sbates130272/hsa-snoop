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

// On some platforms the KFD maps memory inside /dev/dri/renderD* DRM shared
// mappings that process_vm_readv refuses with EFAULT: the write/read queue
// pointers on RDNA4 (gfx1201), and device allocations plus the kernarg pool on
// CDNA3 (gfx942) and CDNA4 (gfx950). /proc/<pid>/mem pread() goes through the
// VMA directly and succeeds on those pages. We keep a small fd cache so the
// hot poll path doesn't open/close on every call.
bool ReadProcMemViaMem(int pid, uint64_t va, void* out, size_t len) {
    // Thread-local fd cache, so no locking is needed. In --all mode the cache
    // thrashes when consecutive reads target different pids; that is a
    // throughput concern only, never a correctness one.
    static __thread int cached_pid = -1;
    static __thread int cached_fd = -1;

    // Reopen when the pid changed OR the previous open failed. Caching a
    // failed open would be permanent: cached_pid would match on every later
    // call and short-circuit to `return false` forever. That was survivable
    // when this path was only an RDNA4 fallback for queue pointers, but it is
    // now the ONLY path that can read the kernarg buffer on gfx942/gfx950, so
    // one transient EMFILE would silently disable --mem-snoop for the rest of
    // the run. Retrying costs an open() per call only while the pid is
    // genuinely unreadable, which the caller bounds by evicting the queue.
    if (cached_pid != pid || cached_fd < 0) {
        if (cached_fd >= 0)
            close(cached_fd);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        cached_fd = open(path, O_RDONLY);
        // Only remember the pid if we actually hold an fd for it.
        cached_pid = (cached_fd >= 0) ? pid : -1;
    }
    if (cached_fd < 0)
        return false;
    return pread(cached_fd, out, len, static_cast<off_t>(va)) ==
           static_cast<ssize_t>(len);
}

// AMDGPU kernel descriptor (code object v3 and later). Only the leading fields
// matter here:
//   offset 0  u32 group_segment_fixed_size
//   offset 4  u32 private_segment_fixed_size
//   offset 8  u32 kernarg_size          <-- added in the v3 descriptor
//   offset 12 u8[4] reserved
// In v2 and earlier, offset 8 is reserved and reads as zero, which the caller
// treats as "unavailable".
constexpr uint32_t kKdKernargSizeOffset = 8;

uint32_t ReadKernargSize(int pid, uint64_t kernel_object) {
    if (!kernel_object)
        return 0;
    uint32_t size = 0;
    const uint64_t va = kernel_object + kKdKernargSizeOffset;
    if (!ReadProcMem(pid, va, &size, sizeof(size)) &&
        !ReadProcMemViaMem(pid, va, &size, sizeof(size)))
        return 0;
    // Reject implausible values: a corrupt or pre-v3 descriptor must not be
    // able to widen the scan window.
    if (size == 0 || size > kMaxKernargScanBytes)
        return 0;
    return size;
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

    uint32_t scan_bytes = (kernarg_bytes < kMaxKernargScanBytes)
                              ? kernarg_bytes
                              : kMaxKernargScanBytes;
    // Align down to 8.
    scan_bytes &= ~7u;
    if (scan_bytes < 8)
        return true;

    // Same try-then-fall-back shape as ReadQueuePtr() in parser.cpp. On gfx942
    // and gfx950 the kernarg pool lives in a renderD-backed mapping, so the
    // process_vm_readv path fails for every dispatch and only the pread path
    // works; on gfx90a and gfx908 the first call succeeds.
    std::vector<uint8_t> buf(scan_bytes);
    if (!ReadProcMem(pid, kernarg_va, buf.data(), scan_bytes) &&
        !ReadProcMemViaMem(pid, kernarg_va, buf.data(), scan_bytes))
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

        // Only count a region when a candidate points at its BASE.
        //
        // Two distinct failure modes make "points anywhere inside a mapping"
        // untenable, both measured on gfx908:
        //
        //  1. An 8-byte window can straddle two adjacent arguments. vadd's
        //     `int n` sits at offset 24 followed by padding, so the read there
        //     fabricated 0x7c6400100000 -- low half n=0x100000, high half the
        //     top bits of the next field. It is page aligned by luck and lands
        //     1.6 GB inside a 3.9 GB anonymous mapping, inflating one
        //     dispatch's footprint from 12 MiB to 3.9 GiB.
        //  2. The implicit-argument area HIP appends holds non-pointer words
        //     that land in unrelated mappings (e.g. 0x7c901cfb95f1 -> a 45 MB
        //     library region).
        //
        // Every genuine device buffer observed pointed at its VMA base exactly,
        // and every false positive pointed into the middle. Since the estimate
        // adds up whole regions, requiring the base is also the self-consistent
        // rule: crediting an entire VMA to a word aimed at its interior is the
        // over-count this is meant to bound.
        //
        // The cost is that a buffer referenced only by an interior pointer is
        // missed. That is deliberate, and it is why mapped_vram is documented
        // as an estimate rather than an exact figure.
        const VmaRegion* v = FindVma(vmas, candidate);
        if (!v || v->start != candidate)
            continue;

        (*ptr_count)++;
        if (seen_starts.insert(v->start).second)
            *mapped_vram_bytes += (v->end - v->start);
    }
    return true;
}

} // namespace hsasnoop
