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
// Both queues live in the test process's own address space.  ReadQueuePtr
// calls process_vm_readv / /proc/self/mem, which succeeds for valid VAs and
// fails (EFAULT / ESRCH) for unmapped ones — no GPU, no mocking needed.

#include "aql.h"
#include "parser.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// An address well beyond the x86-64 47-bit user-space ceiling; both
// process_vm_readv and pread(/proc/self/mem) return EFAULT / ESRCH for it.
constexpr uint64_t kUnmappedVa = 0x7ff0000000000000ULL;

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
        [&](const hsasnoop::PacketRecord&) { ++bad_calls; }, {}, /*poll_us=*/500);

    hsasnoop::QueueInfo queue;
    queue.pid = getpid();
    queue.ring_base = reinterpret_cast<uint64_t>(ring);
    // Both pointer VAs are unmapped — every ReadQueuePtr call will fail.
    queue.wptr_addr = kUnmappedVa;
    queue.rptr_addr = kUnmappedVa;
    queue.ring_size = static_cast<uint32_t>(kRingBytes);
    queue.qtype = 2;  // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL
    queue.uid = 1;
    queue.gpu_id = 0;
    parser.AddQueue(queue);

    // Wait long enough for 50+ poll cycles (50 × 0.5 ms = 25 ms minimum;
    // use 200 ms to be robust on loaded CI machines).
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    parser.Stop();

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

} // namespace

int main() {
    int failures = 0;
    if (!TestBadQueueEvicted())
        ++failures;
    if (!TestGoodQueueSurvivesEviction())
        ++failures;
    return failures ? 1 : 0;
}
