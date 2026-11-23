#define _LARGEFILE64_SOURCE
#include "proc_mem.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
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
namespace {
// Thread-local fd cache, so no locking is needed: each poll thread, and the
// watch thread, has its own. Several entries, because on ROCm 10.2 every
// ring read comes this way and a poller watching more than one process would
// otherwise reopen the file on every switch.
struct MemFdEntry {
    int pid = -1;
    int fd = -1;
};
constexpr unsigned kMemFdEntries = 8;
__thread MemFdEntry mem_fd_cache[kMemFdEntries];
__thread unsigned mem_fd_next_victim = 0;
} // namespace

bool ReadProcMemViaMem(int pid, uint64_t va, void* out, size_t len) {
    MemFdEntry* e = nullptr;
    MemFdEntry* empty = nullptr;
    for (MemFdEntry& c : mem_fd_cache) {
        if (c.fd >= 0 && c.pid == pid) {
            e = &c;
            break;
        }
        if (c.fd < 0 && !empty)
            empty = &c;
    }
    if (!e) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        // A failed open is not remembered: caching it would turn one
        // transient EMFILE into a permanent failure for that pid.
        if (fd < 0)
            return false;
        e = empty;
        if (!e) {
            e = &mem_fd_cache[mem_fd_next_victim];
            mem_fd_next_victim = (mem_fd_next_victim + 1) % kMemFdEntries;
        }
        if (e->fd >= 0)
            close(e->fd);
        e->pid = pid;
        e->fd = fd;
    }
    if (pread(e->fd, out, len, static_cast<off_t>(va)) ==
        static_cast<ssize_t>(len))
        return true;
    // Drop the descriptor on any failure. One held on a process that has
    // exited reads nothing for ever, and if the pid is reused the entry would
    // keep pointing at the dead process.
    close(e->fd);
    e->fd = -1;
    e->pid = -1;
    return false;
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
    // Use off64_t and pread64 so the pagemap offset doesn't truncate for high
    // 64-bit VAs (e.g. 0x7f3398200000 -> ~273 GB into the file).
    off64_t off = static_cast<off64_t>(va / page) * sizeof(uint64_t);
    uint64_t phys = 0;
    if (pread64(fd, &entry, sizeof(entry), off) == sizeof(entry)) {
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

std::string MappingBacking(int pid, uint64_t va) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        uint64_t start = 0, end = 0;
        int name_at = -1;
        // start-end perms offset dev inode [path]
        if (sscanf(line.c_str(), "%lx-%lx %*s %*s %*s %*s %n", &start, &end,
                   &name_at) < 2)
            continue;
        if (va < start || va >= end)
            continue;
        if (name_at < 0 || static_cast<size_t>(name_at) >= line.size())
            return "anonymous";
        return line.substr(name_at);
    }
    return "";
}

namespace {

// PROCMAP_QUERY (Linux 6.11) looks up the mapping that covers one address
// without the whole of /proc/<pid>/maps being formatted and parsed. Declared
// here because the build host's headers may predate it.
struct ProcmapQuery {
    uint64_t size;
    uint64_t query_flags;
    uint64_t query_addr;
    uint64_t vma_start;
    uint64_t vma_end;
    uint64_t vma_flags;
    uint64_t vma_page_size;
    uint64_t vma_offset;
    uint64_t inode;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint32_t vma_name_size;
    uint32_t build_id_size;
    uint64_t vma_name_addr;
    uint64_t build_id_addr;
};
constexpr unsigned long kProcmapQueryIoctl = _IOWR('f', 17, ProcmapQuery);

bool NoSuchProcess(int err) { return err == ENOENT || err == ESRCH; }

} // namespace

Mapped AddressMapped(int pid, uint64_t va) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NoSuchProcess(errno) ? Mapped::No : Mapped::Unknown;
    ProcmapQuery q{};
    q.size = sizeof(q);
    q.query_addr = va;
    if (ioctl(fd, kProcmapQueryIoctl, &q) == 0) {
        close(fd);
        return Mapped::Yes;
    }
    const int err = errno;
    if (err != ENOTTY && err != EINVAL) {
        close(fd);
        // ENOENT: nothing covers the address. ESRCH: the process has gone.
        return NoSuchProcess(err) ? Mapped::No : Mapped::Unknown;
    }
    // An older kernel: read the map. Lines are "start-end ...", ascending.
    std::string text;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        text.append(buf, static_cast<size_t>(n));
    const int read_err = errno;
    close(fd);
    if (n < 0)
        return NoSuchProcess(read_err) ? Mapped::No : Mapped::Unknown;
    const char* p = text.c_str();
    while (*p) {
        char* end = nullptr;
        const uint64_t start = strtoull(p, &end, 16);
        if (end != p && *end == '-') {
            const uint64_t stop = strtoull(end + 1, nullptr, 16);
            if (va >= start && va < stop)
                return Mapped::Yes;
            if (va < start)
                break;
        }
        const char* nl = strchr(p, '\n');
        if (!nl)
            break;
        p = nl + 1;
    }
    return Mapped::No;
}

bool ProcAlive(int pid) {
    // /proc/<pid> outlives the address space: it is there for as long as the
    // process is exiting (which, with GPU queues to tear down, can take a
    // while) and then as a zombie until it is reaped. statm reads all zeroes
    // once the mm has gone.
    //
    // Only a definite answer counts as dead. A queue dropped for a dead
    // process is never picked up again, so running out of descriptors or
    // memory for a moment here must not look like the process exiting.
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return !NoSuchProcess(errno);
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    const int err = errno;
    close(fd);
    if (n < 0)
        return !NoSuchProcess(err);
    if (n == 0)
        return false;
    buf[n] = '\0';
    return strtoull(buf, nullptr, 10) != 0;
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
// vmas from /proc/<pid>/maps arrive in ascending order.
const VmaRegion* FindVma(const std::vector<VmaRegion>& vmas, uint64_t addr) {
    auto it = std::upper_bound(
        vmas.begin(), vmas.end(), addr,
        [](uint64_t a, const VmaRegion& v) { return a < v.start; });
    if (it == vmas.begin())
        return nullptr;
    --it;
    return addr < it->end ? &*it : nullptr;
}

double MonoSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Reading /proc/<pid>/maps costs milliseconds for a process with thousands of
// mappings (PyTorch, vLLM) and takes the target's mmap lock. Doing it per
// dispatch stalled the ring poller, so callers on the dispatch path reuse a
// snapshot for up to max_age_sec.
struct VmaCacheEntry {
    std::vector<VmaRegion> vmas;
    double read_at = 0;
};
std::mutex vma_cache_mu;
std::unordered_map<int, VmaCacheEntry> vma_cache;

std::vector<VmaRegion> GetVmas(int pid, double max_age_sec) {
    if (max_age_sec <= 0)
        return ReadVmas(pid);
    {
        std::lock_guard<std::mutex> lk(vma_cache_mu);
        auto it = vma_cache.find(pid);
        if (it != vma_cache.end() &&
            MonoSec() - it->second.read_at < max_age_sec)
            return it->second.vmas;
    }
    // Not under the lock: this read can block on the target's mmap lock, and
    // must then hold up only the thread that polls that process.
    std::vector<VmaRegion> vmas = ReadVmas(pid);
    std::lock_guard<std::mutex> lk(vma_cache_mu);
    if (vma_cache.size() >= 64 && !vma_cache.count(pid))
        vma_cache.clear();
    VmaCacheEntry& e = vma_cache[pid];
    e.vmas = vmas;
    e.read_at = MonoSec();
    return vmas;
}

} // namespace

bool ScanKernargFootprint(int pid, uint64_t kernarg_va, uint32_t kernarg_bytes,
                          uint64_t* mapped_vram_bytes, uint32_t* ptr_count,
                          double max_vma_age_sec) {
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

    std::vector<VmaRegion> vmas = GetVmas(pid, max_vma_age_sec);
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

void ForgetProcess(int pid) {
    {
        std::lock_guard<std::mutex> lk(vma_cache_mu);
        vma_cache.erase(pid);
    }
    for (MemFdEntry& c : mem_fd_cache)
        if (c.fd >= 0 && c.pid == pid) {
            close(c.fd);
            c.fd = -1;
            c.pid = -1;
        }
}

} // namespace hsasnoop
