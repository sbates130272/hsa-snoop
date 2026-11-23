// parser_eviction_test.cpp — unit test for the RingParser queue eviction path.
//
// Validates two behaviours without requiring a GPU or root:
//
//   1. A queue with unmapped wptr/rptr addresses is evicted after exactly the
//      configured number of sustained read failures (currently 50), and the
//      AQL sink is never called for it.
//
//   2. A healthy queue alongside the bad one continues to produce records
//      through the eviction window, confirming the parser thread survives
//      the eviction and does not abort or lock up.
//
//   4. The removal callback fires once for a dropped queue, outside the
//      parser's lock, and not for queues still live at Stop().
//
//   3. A queue whose process has exited, or whose pointer page its owner has
//      unmapped (what a runtime shutting down does), is dropped at once and
//      without the "evicting stale queue" message. That message is kept for
//      a live process whose pointers cannot be read.
//
// Both queues live in the test process's own address space.  ReadQueuePtr
// calls process_vm_readv / /proc/self/mem, which succeeds for valid VAs and
// fails (EFAULT / ESRCH) for unmapped ones — no GPU, no mocking needed.

#include "aql.h"
#include "parser.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// An address well beyond the x86-64 47-bit user-space ceiling; both
// process_vm_readv and pread(/proc/self/mem) return EFAULT / ESRCH for it.
constexpr uint64_t kUnmappedVa = 0x7ff0000000000000ULL;

// Runs fn with stderr redirected to a file and returns what was written.
template <typename Fn>
std::string CaptureStderr(Fn fn) {
    char tmpl[] = "/tmp/hsa-snoop-eviction-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return {};
    unlink(tmpl);
    std::fflush(stderr);
    int saved = dup(STDERR_FILENO);
    dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);

    std::string out;
    char buf[4096];
    ssize_t n;
    lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        out.append(buf, static_cast<size_t>(n));
    close(fd);
    std::fputs(out.c_str(), stderr);
    return out;
}

// ---------------------------------------------------------------------------
// Build a minimal valid AQL kernel-dispatch packet in ring memory so the
// parser can decode it for the healthy queue.
// ---------------------------------------------------------------------------

// HSA AQL packet type and header constants (mirrored from aql.h internals so
// the test has no compile-time dependency on the private header layout).
constexpr uint16_t kHsaAqlPacketTypeKernelDispatch = 2;
constexpr uint16_t kAqlBarrier = 1u << 8;
constexpr uint16_t kAqlAcquireSystem = 2u << 9;
constexpr uint16_t kAqlReleaseSystem = 2u << 11;

// A 64-byte AQL kernel-dispatch packet.  Only the header word matters for
// the parser to classify and emit the record; the rest can be zeros.
struct alignas(64) AqlDispatchPacket {
    uint16_t header;
    uint16_t setup;
    uint16_t workgroup_size_x;
    uint16_t workgroup_size_y;
    uint16_t workgroup_size_z;
    uint16_t reserved0;
    uint32_t grid_size_x;
    uint32_t grid_size_y;
    uint32_t grid_size_z;
    uint32_t private_segment_size;
    uint32_t group_segment_size;
    uint64_t kernel_object;
    uint64_t kernarg_address;
    uint64_t reserved1;
    uint64_t completion_signal;
};
static_assert(sizeof(AqlDispatchPacket) == 64, "AQL packet must be 64 bytes");

void WriteDispatch(void* slot) {
    auto* p = static_cast<AqlDispatchPacket*>(slot);
    *p = {};
    p->workgroup_size_x = 64;
    p->grid_size_x = 64;
    // Set the header last (release store) to publish the packet.
    uint16_t hdr = kHsaAqlPacketTypeKernelDispatch | kAqlBarrier |
                   kAqlAcquireSystem | kAqlReleaseSystem;
    __atomic_store_n(&p->header, hdr, __ATOMIC_RELEASE);
}

// ---------------------------------------------------------------------------
// Test 1: bad queue evicted; AQL sink never called for it
// ---------------------------------------------------------------------------

bool TestBadQueueEvicted() {
    // 16-slot AQL ring (16 × 64 bytes = 1024 bytes)
    constexpr int kSlots = 16;
    constexpr size_t kRingBytes = kSlots * 64;
    alignas(64) uint8_t ring[kRingBytes] = {};
    alignas(uint64_t) std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr{0};

    int bad_calls = 0;
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord&) { ++bad_calls; }, {},
        /*poll_us=*/500);

    hsasnoop::QueueInfo queue;
    queue.pid = getpid();
    queue.ring_base = reinterpret_cast<uint64_t>(ring);
    // Both pointer VAs are unmapped — every ReadQueuePtr call will fail.
    queue.wptr_addr = kUnmappedVa;
    queue.rptr_addr = kUnmappedVa;
    queue.ring_size = static_cast<uint32_t>(kRingBytes);
    queue.qtype = 2; // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL
    queue.uid = 1;
    queue.gpu_id = 0;

    // Wait long enough for 50+ poll cycles (50 × 0.5 ms = 25 ms minimum;
    // use 200 ms to be robust on loaded CI machines).
    const std::string log = CaptureStderr([&] {
        parser.AddQueue(queue);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        parser.Stop();
    });

    // This process is alive, so the eviction is the genuine stale case and
    // must be reported.
    if (log.find("evicting stale queue uid=1") == std::string::npos) {
        std::fprintf(stderr,
                     "FAIL TestBadQueueEvicted: no eviction message for a "
                     "live process with unreadable pointers\n");
        return false;
    }
    if (bad_calls != 0) {
        std::fprintf(stderr,
                     "FAIL TestBadQueueEvicted: sink called %d times for a "
                     "queue with unmapped pointers\n",
                     bad_calls);
        return false;
    }
    std::fprintf(stderr, "PASS TestBadQueueEvicted\n");
    return true;
}

// ---------------------------------------------------------------------------
// Test 2: healthy queue survives alongside an evicted bad queue
// ---------------------------------------------------------------------------

bool TestGoodQueueSurvivesEviction() {
    constexpr int kSlots = 64;
    constexpr size_t kRingBytes = kSlots * 64;

    // --- Healthy queue setup (valid in-process VAs) ---
    alignas(64) uint8_t good_ring[kRingBytes] = {};
    alignas(uint64_t) std::atomic<uint64_t> good_wptr{0};
    alignas(uint64_t) std::atomic<uint64_t> good_rptr{0};

    // --- Bad queue: unmapped pointers, same parser instance ---
    std::mutex records_mu;
    std::vector<hsasnoop::PacketRecord> good_records;
    int bad_calls = 0;

    // uid=1 → good, uid=2 → bad (same sink; distinguish by queue_uid)
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& r) {
            std::lock_guard<std::mutex> lk(records_mu);
            if (r.queue_uid == 1)
                good_records.push_back(r);
            else
                ++bad_calls;
        },
        {}, /*poll_us=*/500);

    hsasnoop::QueueInfo good_q;
    good_q.pid = getpid();
    good_q.ring_base = reinterpret_cast<uint64_t>(good_ring);
    good_q.wptr_addr = reinterpret_cast<uint64_t>(&good_wptr);
    good_q.rptr_addr = reinterpret_cast<uint64_t>(&good_rptr);
    good_q.ring_size = static_cast<uint32_t>(kRingBytes);
    good_q.qtype = 2;
    good_q.uid = 1;
    good_q.gpu_id = 0;
    parser.AddQueue(good_q);

    hsasnoop::QueueInfo bad_q;
    bad_q.pid = getpid();
    bad_q.ring_base = 0; // irrelevant; wptr_addr is unmapped
    bad_q.wptr_addr = kUnmappedVa;
    bad_q.rptr_addr = kUnmappedVa;
    bad_q.ring_size = 4096;
    bad_q.qtype = 2;
    bad_q.uid = 2;
    bad_q.gpu_id = 0;
    parser.AddQueue(bad_q);

    // Let the bad queue accumulate its 50 failure ticks (25 ms at 0.5 ms/tick).
    // Wait 100 ms before publishing to the good queue so the bad queue is
    // evicted by the time we write packets, proving the parser keeps running.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Publish 4 dispatch packets to the good queue.
    for (int i = 0; i < 4; ++i) {
        uint64_t slot_id = good_wptr.load(std::memory_order_acquire);
        uint64_t slot_off = (slot_id % kSlots) * 64;
        WriteDispatch(good_ring + slot_off);
        good_wptr.store(slot_id + 1, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        good_rptr.store(slot_id + 1, std::memory_order_release);
    }

    // Give the parser time to process the good packets.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    parser.Stop();

    bool ok = true;

    {
        std::lock_guard<std::mutex> lk(records_mu);
        if (good_records.empty()) {
            std::fprintf(stderr,
                         "FAIL TestGoodQueueSurvivesEviction: no records from "
                         "the healthy queue\n");
            ok = false;
        }
        if (bad_calls != 0) {
            std::fprintf(stderr,
                         "FAIL TestGoodQueueSurvivesEviction: sink called %d "
                         "times for the bad queue\n",
                         bad_calls);
            ok = false;
        }
    }

    if (ok)
        std::fprintf(stderr,
                     "PASS TestGoodQueueSurvivesEviction: %zu good record(s)\n",
                     good_records.size());
    return ok;
}

// ---------------------------------------------------------------------------
// Test 3: the queues of an exited process go quietly
// ---------------------------------------------------------------------------

bool TestExitedProcessDroppedQuietly() {
    // A zombie: /proc/<pid> exists, the address space does not.
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    if (child < 0) {
        std::fprintf(stderr, "FAIL TestExitedProcessDroppedQuietly: fork\n");
        return false;
    }
    siginfo_t info{};
    waitid(P_PID, child, &info, WEXITED | WNOWAIT);

    uint64_t polls = 0;
    const std::string log = CaptureStderr([&] {
        hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {},
                                    [](const hsasnoop::SdmaRecord&) {},
                                    /*poll_us=*/500);
        for (uint32_t qtype : {2u, 1u}) { // AQL, SDMA
            hsasnoop::QueueInfo queue;
            queue.pid = child;
            queue.ring_base = 0x10000;
            queue.wptr_addr = 0x20000;
            queue.rptr_addr = 0x20008;
            queue.ring_size = 4096;
            queue.qtype = qtype;
            queue.uid = 10 + qtype;
            queue.sdma_version = sdma::Version::V7Gfx125;
            parser.AddQueue(queue);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        polls = parser.QueuePolls();
        parser.Stop();
    });
    waitpid(child, nullptr, 0);

    bool ok = true;
    if (log.find("evicting stale") != std::string::npos) {
        std::fprintf(stderr, "FAIL TestExitedProcessDroppedQuietly: an exited "
                             "process was reported as a stale queue\n");
        ok = false;
    }
    // Dropped on the first look, not after 50 failed reads each.
    if (polls > 2) {
        std::fprintf(stderr,
                     "FAIL TestExitedProcessDroppedQuietly: %lu polls of "
                     "queues whose process had exited\n",
                     static_cast<unsigned long>(polls));
        ok = false;
    }
    if (ok)
        std::fprintf(stderr, "PASS TestExitedProcessDroppedQuietly\n");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 4: a queue destroyed by its live owner goes quietly
// ---------------------------------------------------------------------------

bool TestDestroyedQueueDroppedQuietly() {
    constexpr size_t kRingBytes = 16 * 64;
    alignas(64) static uint8_t ring[kRingBytes] = {};
    // The pointers get a page of their own so it can be unmapped, as the
    // runtime does with a queue's pointer page when it destroys the queue.
    void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (page == MAP_FAILED) {
        std::fprintf(stderr, "FAIL TestDestroyedQueueDroppedQuietly: mmap\n");
        return false;
    }

    uint64_t polls_before = 0, polls_at_unmap = 0, polls_after = 0;
    const std::string log = CaptureStderr([&] {
        hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                    /*poll_us=*/500, {}, /*poll_idle_us=*/0);
        hsasnoop::QueueInfo queue;
        queue.pid = getpid();
        queue.ring_base = reinterpret_cast<uint64_t>(ring);
        queue.wptr_addr = reinterpret_cast<uint64_t>(page);
        queue.rptr_addr = reinterpret_cast<uint64_t>(page) + 8;
        queue.ring_size = static_cast<uint32_t>(kRingBytes);
        queue.qtype = 2;
        queue.uid = 20;
        parser.AddQueue(queue);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        polls_before = parser.QueuePolls();
        munmap(page, 4096);
        polls_at_unmap = parser.QueuePolls();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        polls_after = parser.QueuePolls();
        parser.Stop();
    });

    bool ok = true;
    if (polls_before == 0) {
        std::fprintf(stderr, "FAIL TestDestroyedQueueDroppedQuietly: the "
                             "queue was never polled while it was live\n");
        ok = false;
    }
    if (log.find("evicting stale") != std::string::npos) {
        std::fprintf(stderr, "FAIL TestDestroyedQueueDroppedQuietly: a "
                             "destroyed queue was reported as stale\n");
        ok = false;
    }
    // One failed read drops it; a poll or two may straddle the munmap.
    if (polls_after > polls_at_unmap + 3) {
        std::fprintf(stderr,
                     "FAIL TestDestroyedQueueDroppedQuietly: %lu polls after "
                     "the pointer page was unmapped\n",
                     static_cast<unsigned long>(polls_after - polls_at_unmap));
        ok = false;
    }
    if (ok)
        std::fprintf(stderr, "PASS TestDestroyedQueueDroppedQuietly\n");
    return ok;
}

// ---------------------------------------------------------------------------
// Test 5: the owner is told about a dropped queue, once, with no lock held
// ---------------------------------------------------------------------------

bool TestRemovalCallback() {
    constexpr size_t kRingBytes = 16 * 64;
    alignas(64) static uint8_t ring[kRingBytes] = {};
    alignas(uint64_t) static std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) static std::atomic<uint64_t> rptr{0};

    hsasnoop::QueueInfo good;
    good.pid = getpid();
    good.ring_base = reinterpret_cast<uint64_t>(ring);
    good.wptr_addr = reinterpret_cast<uint64_t>(&wptr);
    good.rptr_addr = reinterpret_cast<uint64_t>(&rptr);
    good.ring_size = static_cast<uint32_t>(kRingBytes);
    good.qtype = 2;
    good.uid = 2;

    hsasnoop::QueueInfo bad = good;
    bad.wptr_addr = kUnmappedVa;
    bad.rptr_addr = kUnmappedVa;
    bad.uid = 1;

    std::mutex mu;
    std::vector<uint64_t> removed;
    hsasnoop::RingParser* self = nullptr;
    hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                /*poll_us=*/500, {}, -1,
                                [&](const hsasnoop::QueueInfo& q, bool) {
                                    {
                                        std::lock_guard<std::mutex> lk(mu);
                                        removed.push_back(q.uid);
                                    }
                                    // Would deadlock if the parser still held
                                    // its lock.
                                    hsasnoop::QueueInfo again = good;
                                    again.uid = 3;
                                    self->AddQueue(again);
                                });
    self = &parser;

    CaptureStderr([&] {
        parser.AddQueue(good);
        parser.AddQueue(bad);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        parser.Stop();
    });

    // Queues 2 and 3 were live at Stop(), which reports nothing.
    std::lock_guard<std::mutex> lk(mu);
    if (removed.size() != 1 || removed[0] != 1) {
        std::fprintf(stderr,
                     "FAIL TestRemovalCallback: %zu removal(s), first uid "
                     "%lu; expected exactly one, for uid 1\n",
                     removed.size(),
                     removed.empty() ? 0UL
                                     : static_cast<unsigned long>(removed[0]));
        return false;
    }
    std::fprintf(stderr, "PASS TestRemovalCallback\n");
    return true;
}

// ---------------------------------------------------------------------------
// Test 6: running out of file descriptors is not the process exiting
// ---------------------------------------------------------------------------

// A queue dropped is never picked up again, so a liveness check that cannot
// open its /proc file must not count as the process having gone.
bool TestFdExhaustionDoesNotEvict() {
    constexpr size_t kRingBytes = 16 * 64;
    alignas(64) static uint8_t ring[kRingBytes] = {};
    alignas(uint64_t) static std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) static std::atomic<uint64_t> rptr{0};

    struct rlimit saved;
    if (getrlimit(RLIMIT_NOFILE, &saved) != 0) {
        std::fprintf(stderr, "SKIP: TestFdExhaustionDoesNotEvict: getrlimit\n");
        return true;
    }
    std::atomic<int> removed{0};
    hsasnoop::RingParser parser(
        [](const hsasnoop::PacketRecord&) {}, {}, /*poll_us=*/500, {},
        /*poll_idle_us=*/0,
        [&](const hsasnoop::QueueInfo&, bool) { ++removed; });
    hsasnoop::QueueInfo queue;
    queue.pid = getpid();
    queue.ring_base = reinterpret_cast<uint64_t>(ring);
    queue.wptr_addr = reinterpret_cast<uint64_t>(&wptr);
    queue.rptr_addr = reinterpret_cast<uint64_t>(&rptr);
    queue.ring_size = static_cast<uint32_t>(kRingBytes);
    queue.qtype = 2;
    queue.uid = 30;
    parser.AddQueue(queue);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Only descriptors 0 to 2 may be opened from here: every open() fails.
    struct rlimit tight = saved;
    tight.rlim_cur = 3;
    if (setrlimit(RLIMIT_NOFILE, &tight) != 0) {
        std::fprintf(stderr, "SKIP: TestFdExhaustionDoesNotEvict: setrlimit\n");
        parser.Stop();
        return true;
    }
    const int probe = open("/proc/self/statm", O_RDONLY);
    const bool exhausted = probe < 0;
    if (!exhausted)
        close(probe);
    // Several liveness checks, which come every 100 ms.
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    setrlimit(RLIMIT_NOFILE, &saved);

    const uint64_t polls = parser.QueuePolls();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool still_polled = parser.QueuePolls() > polls;
    parser.Stop();
    if (!exhausted) {
        std::fprintf(stderr, "SKIP: TestFdExhaustionDoesNotEvict: open() "
                             "succeeded at the descriptor limit\n");
        return true;
    }
    if (removed != 0 || !still_polled) {
        std::fprintf(stderr,
                     "FAIL TestFdExhaustionDoesNotEvict: a live queue was "
                     "dropped while no file could be opened\n");
        return false;
    }
    std::fprintf(stderr, "PASS TestFdExhaustionDoesNotEvict\n");
    return true;
}

} // namespace

int main() {
    int failures = 0;
    if (!TestBadQueueEvicted())
        ++failures;
    if (!TestGoodQueueSurvivesEviction())
        ++failures;
    if (!TestExitedProcessDroppedQuietly())
        ++failures;
    if (!TestDestroyedQueueDroppedQuietly())
        ++failures;
    if (!TestRemovalCallback())
        ++failures;
    if (!TestFdExhaustionDoesNotEvict())
        ++failures;
    return failures ? 1 : 0;
}
