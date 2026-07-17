#include "ksym.h"

#include <cxxabi.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace hsasnoop {
namespace {

#ifndef EM_AMDGPU
#define EM_AMDGPU 224
#endif

// Reads target memory via /proc/<pid>/mem. Unlike process_vm_readv, this path
// can read AMDGPU code objects that are backed by device memory (/dev/dri/
// renderD128, mapped VM_PFNMAP) where the kernel descriptors actually live.
struct MemReader {
    int fd = -1;
    explicit MemReader(int pid) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/mem", pid);
        fd = open(path, O_RDONLY);
    }
    ~MemReader() {
        if (fd >= 0)
            close(fd);
    }
    bool Read(uint64_t va, void* out, size_t len) const {
        if (fd < 0)
            return false;
        return pread(fd, out, len, static_cast<off_t>(va)) ==
               static_cast<ssize_t>(len);
    }
};

// Read a POD value from target memory.
template <typename T>
bool Peek(const MemReader& m, uint64_t va, T* out) {
    return m.Read(va, out, sizeof(T));
}

// Read a NUL-terminated string (bounded) from target memory.
std::string PeekStr(const MemReader& m, uint64_t va, size_t max = 512) {
    std::string s;
    char buf[64];
    while (s.size() < max) {
        if (!m.Read(va + s.size(), buf, sizeof(buf)))
            break;
        for (size_t i = 0; i < sizeof(buf); ++i) {
            if (buf[i] == '\0')
                return s;
            s.push_back(buf[i]);
        }
    }
    return s;
}

// Candidate base addresses of ELF images mapped in the process.
std::vector<uint64_t> FindElfBases(int pid, const MemReader& m) {
    std::vector<uint64_t> bases;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        // Format: start-end perms offset dev inode path
        uint64_t start = 0, end = 0, off = 1;
        char perms[8] = {0};
        if (sscanf(line.c_str(), "%lx-%lx %7s %lx", &start, &end, perms, &off) <
            4)
            continue;
        if (perms[0] != 'r')
            continue;
        // NB: AMDGPU code objects backed by /dev/dri/renderD128 report a
        // nonzero file offset (renderD128 is one large BO), yet each object's
        // ELF header still sits at the mapping start. So we probe every
        // readable region start for ELF magic rather than filtering on offset
        // 0.
        unsigned char ident[20];
        if (!m.Read(start, ident, sizeof(ident)))
            continue;
        if (memcmp(ident, ELFMAG, SELFMAG) != 0)
            continue;
        uint16_t machine = *reinterpret_cast<uint16_t*>(ident + 18);
        if (machine == EM_AMDGPU)
            bases.push_back(start);
    }
    return bases;
}

} // namespace

std::string Demangle(const std::string& mangled) {
    int status = 0;
    char* d = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && d) {
        std::string out(d);
        free(d);
        return out;
    }
    return mangled;
}

// Walk one AMDGPU ELF image (loaded at `base`) via its dynamic segment and add
// every "<name>.kd" symbol to the cache as kernel_object -> demangled name.
// Also populates offset_cache (keyed by runtime_va & kOffsetMask) for the
// WSL2/APU case where the GPU VA and CPU VA of the same code object differ.
static void ScanImage(const MemReader& pid, uint64_t base,
                      std::unordered_map<uint64_t, std::string>* cache,
                      std::unordered_map<uint64_t, std::string>* offset_cache,
                      uint64_t offset_mask) {
    Elf64_Ehdr eh;
    if (!Peek(pid, base, &eh))
        return;
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
        return;

    // Program headers (loaded within the first segment).
    uint64_t min_vaddr = UINT64_MAX;
    uint64_t dyn_vaddr = 0, dyn_sz = 0;
    for (int i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (!Peek(pid, base + eh.e_phoff + i * sizeof(ph), &ph))
            return;
        if (ph.p_type == PT_LOAD && ph.p_vaddr < min_vaddr)
            min_vaddr = ph.p_vaddr;
        if (ph.p_type == PT_DYNAMIC) {
            dyn_vaddr = ph.p_vaddr;
            dyn_sz = ph.p_memsz;
        }
    }
    if (min_vaddr == UINT64_MAX)
        min_vaddr = 0;
    const uint64_t bias = base - min_vaddr; // ET_DYN load bias
    if (!dyn_vaddr)
        return;

    // Parse the dynamic array for symtab/strtab pointers.
    uint64_t symtab = 0, strtab = 0, syment = sizeof(Elf64_Sym);
    uint64_t n = dyn_sz / sizeof(Elf64_Dyn);
    for (uint64_t i = 0; i < n; ++i) {
        Elf64_Dyn d;
        if (!Peek(pid, bias + dyn_vaddr + i * sizeof(d), &d))
            break;
        if (d.d_tag == DT_NULL)
            break;
        switch (d.d_tag) {
        case DT_SYMTAB:
            symtab = d.d_un.d_ptr;
            break;
        case DT_STRTAB:
            strtab = d.d_un.d_ptr;
            break;
        case DT_SYMENT:
            syment = d.d_un.d_val;
            break;
        default:
            break;
        }
    }
    if (!symtab || !strtab || !syment)
        return;

    // Dynamic pointers may be link-time vaddrs; rebase if below the load
    // address.
    auto rebase = [&](uint64_t p) { return p < base ? p + bias : p; };
    symtab = rebase(symtab);
    strtab = rebase(strtab);

    // The string table conventionally follows the symbol table in memory; use
    // that to bound the symbol count (dynsym has no explicit count field).
    if (strtab <= symtab)
        return;
    uint64_t count = (strtab - symtab) / syment;
    if (count == 0 || count > 200000)
        return;

    for (uint64_t i = 0; i < count; ++i) {
        Elf64_Sym sym;
        if (!Peek(pid, symtab + i * syment, &sym))
            break;
        if (sym.st_name == 0 || sym.st_value == 0)
            continue;
        std::string name = PeekStr(pid, strtab + sym.st_name);
        if (name.size() < 4)
            continue;
        if (name.compare(name.size() - 3, 3, ".kd") != 0)
            continue;
        uint64_t runtime = sym.st_value + bias;
        std::string demangled = Demangle(name.substr(0, name.size() - 3));
        (*cache)[runtime] = demangled;
        // Low-bit fallback for WSL2/APU: GPU VAs of code objects differ from
        // CPU VAs, but the offset within the code object is the same. Store
        // by low bits so Resolve can match GPU VAs that miss the primary cache.
        // If two different .kd symbols share the same low bits, mark the entry
        // as ambiguous (empty string) so Resolve skips it rather than returning
        // the wrong name.
        if (offset_cache) {
            uint64_t key = runtime & offset_mask;
            auto [oit, inserted] = offset_cache->emplace(key, demangled);
            if (!inserted && oit->second != demangled && !oit->second.empty())
                oit->second = {}; // collision: mark ambiguous
        }
    }
}

void KernelSymbolResolver::ScanCodeObjects() {
    MemReader mem(pid_);
    auto bases = FindElfBases(pid_, mem);
    size_t before = cache_.size();
    for (uint64_t base : bases)
        ScanImage(mem, base, &cache_, &offset_cache_, kOffsetMask);
    ++scanned_generation_;
    if (getenv("HSA_SNOOP_DEBUG"))
        fprintf(stderr,
                "[ksym pid=%d] scanned %zu AMDGPU images, %zu .kd symbols "
                "(+%zu)\n",
                pid_, bases.size(), cache_.size(), cache_.size() - before);
}

std::string KernelSymbolResolver::Resolve(uint64_t kernel_object) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cache_.find(kernel_object);
    if (it != cache_.end())
        return it->second;

    // Miss: (re)scan loaded code objects once and retry.
    ScanCodeObjects();
    it = cache_.find(kernel_object);
    if (it != cache_.end())
        return it->second;

    // WSL2/APU fallback: GPU VAs of code objects differ from CPU VAs, but the
    // intra-object offset (low bits) is the same. Try matching by low bits.
    // Skip ambiguous entries (empty string = two .kd symbols share the same
    // low bits; we can't tell which is correct).
    auto oit = offset_cache_.find(kernel_object & kOffsetMask);
    if (oit != offset_cache_.end() && !oit->second.empty()) {
        // Populate the primary cache so future lookups are O(1).
        cache_[kernel_object] = oit->second;
        return oit->second;
    }

    if (getenv("HSA_SNOOP_DEBUG")) {
        fprintf(stderr,
                "[ksym pid=%d] MISS kernel_object=0x%lx; sample cache:\n", pid_,
                kernel_object);
        int n = 0;
        for (auto& kv : cache_) {
            fprintf(stderr, "    0x%lx -> %s\n", kv.first, kv.second.c_str());
            if (++n >= 6)
                break;
        }
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "kernel_0x%lx", kernel_object);
    // Cache the hex label so the same unresolvable address doesn't trigger
    // another ScanCodeObjects() scan on the next call.
    cache_[kernel_object] = buf;
    return buf;
}

} // namespace hsasnoop
