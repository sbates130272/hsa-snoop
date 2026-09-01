// Unit tests for the cross-process read helpers and the kernarg footprint
// scan. These run against the test process's OWN address space, so they need
// no GPU, no ROCm and no root: process_vm_readv and /proc/self/mem both work
// on your own pid.
//
// The scan's failure contract is what matters most here. ScanKernargFootprint
// zeroes its out-params on entry and returns false when it cannot read, so a
// caller that ignores the return value silently turns "could not measure" into
// "measured zero" -- which is exactly the bug this test exists to catch.

#include "proc_mem.h"

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace hsasnoop;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// An address that is virtually certain not to be mapped, used to provoke the
// read-failure paths. Above the 47-bit user VA ceiling on x86-64.
constexpr uint64_t kUnmappedVa = 0x7ff0000000000000ULL;

// Allocate a region that is genuinely the start of its own VMA. posix_memalign
// is not enough: glibc offsets the returned pointer from the mmap base, so it
// lands inside the mapping rather than at its start. Returns nullptr if the
// kernel merged our mapping into a neighbouring VMA, in which case the caller
// skips rather than reports a false failure.
void* MapOwnVma(size_t len) {
    // Map one extra page and revoke access to it. Without that guard the
    // kernel coalesces successive anonymous mappings with identical flags into
    // a single VMA, so only the lowest allocation would still be a VMA start
    // by the time the scan runs.
    const size_t kGuard = 4096;
    void* p = mmap(nullptr, len + kGuard, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    if (mprotect(static_cast<char*>(p) + len, kGuard, PROT_NONE) != 0) {
        munmap(p, len + kGuard);
        return nullptr;
    }
    std::memset(p, 0, len);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", getpid());
    FILE* f = fopen(path, "r");
    if (!f) {
        munmap(p, len + kGuard);
        return nullptr;
    }
    char line[512];
    bool is_start = false;
    while (fgets(line, sizeof(line), f)) {
        uint64_t s = 0, e = 0;
        if (sscanf(line, "%lx-%lx", &s, &e) == 2 &&
            s == reinterpret_cast<uint64_t>(p)) {
            is_start = true;
            break;
        }
    }
    fclose(f);
    if (!is_start) {
        munmap(p, len + kGuard);
        return nullptr;
    }
    return p;
}

void TestReadProcMem() {
    const int pid = getpid();
    uint64_t src = 0xfeedfacedeadbeefULL;
    uint64_t dst = 0;

    Check(ReadProcMem(pid, reinterpret_cast<uint64_t>(&src), &dst, sizeof(dst)),
          "ReadProcMem should read own memory");
    Check(dst == src, "ReadProcMem should return the correct bytes");

    dst = 0;
    Check(!ReadProcMem(pid, kUnmappedVa, &dst, sizeof(dst)),
          "ReadProcMem should fail on an unmapped address");
}

void TestReadProcMemViaMem() {
    const int pid = getpid();
    uint64_t src = 0x0123456789abcdefULL;
    uint64_t dst = 0;

    Check(ReadProcMemViaMem(pid, reinterpret_cast<uint64_t>(&src), &dst,
                            sizeof(dst)),
          "ReadProcMemViaMem should read own memory");
    Check(dst == src, "ReadProcMemViaMem should return the correct bytes");

    // The length generalisation is the new part: the old helper only ever read
    // 8 bytes. Exercise a buffer larger than one u64 and larger than a page, so
    // a short read or a bad length argument would show up.
    const size_t kBig = 8192;
    std::vector<uint8_t> big_src(kBig);
    for (size_t i = 0; i < kBig; ++i)
        big_src[i] = static_cast<uint8_t>(i * 7 + 1);
    std::vector<uint8_t> big_dst(kBig, 0);

    Check(ReadProcMemViaMem(pid, reinterpret_cast<uint64_t>(big_src.data()),
                            big_dst.data(), kBig),
          "ReadProcMemViaMem should read a multi-page buffer");
    Check(std::memcmp(big_src.data(), big_dst.data(), kBig) == 0,
          "ReadProcMemViaMem multi-page contents should match");

    Check(!ReadProcMemViaMem(pid, kUnmappedVa, &dst, sizeof(dst)),
          "ReadProcMemViaMem should fail on an unmapped address");

    // Regression guard: a failed open() or a failed read must not poison the
    // thread-local fd cache. Caching the failure would make every later call
    // for this pid return false forever, silently disabling --mem-snoop on the
    // parts where the pread path is the only one that works.
    dst = 0;
    Check(ReadProcMemViaMem(pid, reinterpret_cast<uint64_t>(&src), &dst,
                            sizeof(dst)),
          "ReadProcMemViaMem must still work after a failed read");
    Check(dst == src, "ReadProcMemViaMem post-failure contents should match");
}

void TestScanKernargFootprint() {
    const int pid = getpid();
    uint64_t mapped = 0;
    uint32_t ptrs = 0;

    // A synthetic kernarg block holding pointers to three distinct allocations,
    // in the same shape a real dispatch would have. They must be PAGE ALIGNED
    // to model real GPU buffer objects: the scan deliberately ignores
    // unaligned candidates, so plain std::vector heap pointers would (rightly)
    // be rejected and would not exercise the counting path at all.
    void* bufs[3] = {};
    for (auto& b : bufs) {
        b = MapOwnVma(1 << 20);
        if (!b) {
            std::fprintf(stderr, "SKIP: could not map a standalone VMA\n");
            return;
        }
    }

    uint64_t kernarg[8] = {};
    kernarg[0] = reinterpret_cast<uint64_t>(bufs[0]);
    kernarg[1] = reinterpret_cast<uint64_t>(bufs[1]);
    kernarg[2] = reinterpret_cast<uint64_t>(bufs[2]);
    kernarg[3] = 4096; // a plain integer argument, must not count
    kernarg[4] = 0;    // a null pointer, must not count

    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(kernarg),
                               sizeof(kernarg), &mapped, &ptrs),
          "ScanKernargFootprint should succeed on readable memory");
    // The heuristic resolves each pointer to its backing VMA. Heap allocations
    // may share one VMA, so assert on the pointer count and on a non-zero
    // footprint rather than on an exact byte total.
    Check(ptrs >= 3, "ScanKernargFootprint should find at least 3 pointers");
    Check(mapped > 0,
          "ScanKernargFootprint should report a non-zero footprint");

    // Failure contract: an unreadable kernarg address must return false, not a
    // successful measurement of zero.
    mapped = 12345;
    ptrs = 99;
    Check(!ScanKernargFootprint(pid, kUnmappedVa, 256, &mapped, &ptrs),
          "ScanKernargFootprint must return false on an unreadable kernarg");
    Check(mapped == 0 && ptrs == 0,
          "ScanKernargFootprint must zero its out-params on failure");

    // A null kernarg address is "nothing to scan", which is a successful
    // measurement of zero rather than a read failure.
    mapped = 1;
    ptrs = 1;
    Check(ScanKernargFootprint(pid, 0, 256, &mapped, &ptrs),
          "ScanKernargFootprint should treat a null kernarg VA as success");
    Check(mapped == 0 && ptrs == 0,
          "ScanKernargFootprint null-VA case should report zero");

    // Mappings are left in place; the test process exits immediately after.
}

// The scan counts a region only when a candidate points at its BASE. Words
// aiming into the middle of a mapping are rejected: that is what an 8-byte
// window straddling two arguments produces, and it is how one dispatch came to
// report 3.9 GiB instead of 12 MiB.
void TestInteriorPointersIgnored() {
    const int pid = getpid();
    const size_t kLen = 4u << 20;
    void* region = MapOwnVma(kLen);
    if (!region) {
        std::fprintf(stderr, "SKIP: could not map a standalone VMA\n");
        return;
    }
    const uint64_t base = reinterpret_cast<uint64_t>(region);

    uint64_t at_base[4] = {base, 0, 0, 0};
    uint64_t mapped_base = 0, mapped_mid = 0, mapped_odd = 0;
    uint32_t ptrs_base = 0, ptrs_mid = 0, ptrs_odd = 0;
    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(at_base),
                               sizeof(at_base), &mapped_base, &ptrs_base),
          "scan of a base pointer should succeed");

    // Page-aligned, same mapping, but not the base -- this is the shape of the
    // straddled-argument false positive.
    uint64_t interior[4] = {base + 4096, 0, 0, 0};
    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(interior),
                               sizeof(interior), &mapped_mid, &ptrs_mid),
          "scan of an interior pointer should succeed");

    uint64_t unaligned[4] = {base + 0x11, 0, 0, 0};
    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(unaligned),
                               sizeof(unaligned), &mapped_odd, &ptrs_odd),
          "scan of an unaligned candidate should succeed");

    Check(ptrs_base == 1 && mapped_base >= kLen,
          "a pointer to the region base must be counted");
    Check(ptrs_mid == 0 && mapped_mid == 0,
          "a page-aligned INTERIOR pointer must not be counted");
    Check(ptrs_odd == 0 && mapped_odd == 0,
          "an unaligned candidate must not be counted");
}

// The scan must never read past the declared kernarg size. Kernarg blocks are
// packed into a shared pool, so over-reading attributes the NEXT kernel's
// buffers to this dispatch.
void TestScanRespectsDeclaredSize() {
    const int pid = getpid();
    void* victim = MapOwnVma(1 << 20);
    if (!victim) {
        std::fprintf(stderr, "SKIP: could not map a standalone VMA\n");
        return;
    }

    // The same region-base pointer twice: once inside the declared window,
    // once past it.
    uint64_t block[8] = {};
    block[0] = reinterpret_cast<uint64_t>(victim);
    block[4] = block[0]; // at byte offset 32

    uint64_t mapped_small = 0, mapped_big = 0;
    uint32_t ptrs_small = 0, ptrs_big = 0;
    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(block), 16,
                               &mapped_small, &ptrs_small),
          "bounded scan should succeed");
    Check(ScanKernargFootprint(pid, reinterpret_cast<uint64_t>(block),
                               sizeof(block), &mapped_big, &ptrs_big),
          "full scan should succeed");
    Check(ptrs_small == 1, "a 16-byte window must see only the first pointer");
    // Both refer to the same region, so only the first is a fresh count; the
    // pointer COUNT still distinguishes the two window sizes.
    Check(ptrs_big == 2, "the full window should see both pointers");
}

void TestDeadProcess() {
    // Reads against a pid that does not exist must fail, not succeed with
    // stale data. Uses a pid well above the typical pid_max default.
    const int dead_pid = 0x7ffffff;
    uint64_t dst = 0;
    Check(!ReadProcMem(dead_pid, 0x1000, &dst, sizeof(dst)),
          "ReadProcMem should fail for a nonexistent pid");
    Check(!ReadProcMemViaMem(dead_pid, 0x1000, &dst, sizeof(dst)),
          "ReadProcMemViaMem should fail for a nonexistent pid");

    // And the cache must recover for a valid pid afterwards.
    uint64_t src = 0xabcdef0123456789ULL;
    Check(ReadProcMemViaMem(getpid(), reinterpret_cast<uint64_t>(&src), &dst,
                            sizeof(dst)),
          "ReadProcMemViaMem must recover after a nonexistent pid");
    Check(dst == src, "ReadProcMemViaMem recovery contents should match");
}

// Regression test for negative fd caching in ReadProcMemViaMem's thread-local
// cache. If a failed open() is remembered against the pid, every later call for
// that SAME pid short-circuits to false forever -- which would silently disable
// --mem-snoop for the rest of the run on gfx942/gfx950, where the pread path is
// the only one that can reach the kernarg buffer.
//
// Note this must reuse the SAME pid across the failure and the recovery: a test
// that fails on pid A and then reads pid B passes even with the bug present,
// because the differing pid forces a reopen either way.
void TestOpenFailureIsNotCached() {
    const int pid = getpid();
    uint64_t src = 0x5555aaaa5555aaaaULL;
    uint64_t dst = 0;

    // Evict any cached fd for our own pid first. Without this the cache is
    // still warm from earlier tests, ReadProcMemViaMem never calls open(), and
    // the EMFILE window below is never actually exercised.
    uint64_t scratch = 0;
    ReadProcMemViaMem(0x7ffffff, 0x1000, &scratch, sizeof(scratch));

    struct rlimit saved;
    if (getrlimit(RLIMIT_NOFILE, &saved) != 0) {
        std::fprintf(stderr, "SKIP: getrlimit failed\n");
        return;
    }

    // Leave only stdin/stdout/stderr, so any further open() fails with EMFILE.
    struct rlimit tight = saved;
    tight.rlim_cur = 3;
    if (setrlimit(RLIMIT_NOFILE, &tight) != 0) {
        std::fprintf(stderr, "SKIP: could not lower RLIMIT_NOFILE\n");
        return;
    }

    const bool failed_as_expected = !ReadProcMemViaMem(
        pid, reinterpret_cast<uint64_t>(&src), &dst, sizeof(dst));

    // Restore the limit; open() can succeed again from here on.
    if (setrlimit(RLIMIT_NOFILE, &saved) != 0) {
        std::fprintf(stderr, "FAIL: could not restore RLIMIT_NOFILE\n");
        ++failures;
        return;
    }

    if (!failed_as_expected) {
        // The environment kept an fd available, so the failure path never ran.
        std::fprintf(stderr,
                     "SKIP: open() unexpectedly succeeded under EMFILE\n");
        return;
    }

    dst = 0;
    Check(ReadProcMemViaMem(pid, reinterpret_cast<uint64_t>(&src), &dst,
                            sizeof(dst)),
          "ReadProcMemViaMem must retry open() for the same pid after EMFILE");
    Check(dst == src, "ReadProcMemViaMem post-EMFILE contents should match");
}

} // namespace

int main() {
    TestReadProcMem();
    TestReadProcMemViaMem();
    TestScanKernargFootprint();
    TestInteriorPointersIgnored();
    TestScanRespectsDeclaredSize();
    TestDeadProcess();
    TestOpenFailureIsNotCached();

    if (failures) {
        std::fprintf(stderr, "%d proc_mem check(s) failed\n", failures);
        return 1;
    }
    std::printf("all proc_mem checks passed\n");
    return 0;
}
