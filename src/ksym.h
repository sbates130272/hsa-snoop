// ksym.h - Resolve an AQL kernel_object handle to a human-readable kernel name.
//
// The kernel_object in a dispatch packet is the runtime address of the kernel
// descriptor (the ELF symbol "<mangled-name>.kd") inside an AMDGPU code object
// loaded in the target process. We locate AMDGPU (EM_AMDGPU) ELF images in the
// process address space and walk their dynamic symbol tables from memory to
// build a kernel_object -> name map. Best-effort: falls back to a hex label.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hsasnoop {

class KernelSymbolResolver {
  public:
    explicit KernelSymbolResolver(int pid) : pid_(pid) {}

    // Returns a demangled kernel name for kernel_object, or "kernel_0x..." if
    // it cannot be resolved. Results are cached; a miss triggers a rescan of
    // the process's loaded code objects, and an unresolved address is retried
    // a bounded number of times on later calls.
    std::string Resolve(uint64_t kernel_object);

  private:
    void ScanCodeObjects();
    void SettleUnresolved(bool charge_retry);

    int pid_;
    std::mutex mu_;
    std::unordered_map<uint64_t, std::string> cache_; // kernel_object -> name
    // WSL2/APU fallback: GPU VAs differ from CPU VAs for the same code object,
    // but the low bits (intra-object offset) are identical. Maps
    // (kernel_object & kOffsetMask) -> name for fallback lookup.
    // An empty string value signals an ambiguous mapping (two different .kd
    // symbols share the same low bits); ambiguous entries are skipped in
    // Resolve to avoid returning the wrong name.
    // 1 MB: wider than the original 4 KB to handle gfx1250/RDNA4 code objects
    // where the CPU-GPU VA gap can exceed one page. Ambiguity detection (empty
    // string for collisions) prevents false matches from the wider window.
    static constexpr uint64_t kOffsetMask =
        (1ULL << 20) - 1; // 1 MB intra-object offset
    std::unordered_map<uint64_t, std::string>
        offset_cache_; // low bits -> name (empty = ambiguous)
    int scanned_generation_ = 0;
    // Addresses cached under a hex label, with the number of retry scans
    // already charged to each. A miss can be transient (the code object was
    // mid-load or unreadable at that instant), so it is retried a few times.
    // A scan is expensive and runs on the ring-poll thread, so retries are
    // rate-limited and one scan is charged to every pending address: a
    // resolver does at most kMaxRetries retry scans per batch of misses.
    static constexpr int kMaxRetries = 3;
    static constexpr double kRetryIntervalSec = 30.0;
    std::unordered_map<uint64_t, int> unresolved_;
    double last_scan_ts_ = 0;
};

std::string Demangle(const std::string& mangled);

} // namespace hsasnoop
