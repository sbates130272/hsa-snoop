// parser_decode_test.cpp — unit test for two RingParser decode paths that need
// no GPU: packets seen only after the command processor consumed them (header
// reset to Invalid), and where polling starts on a queue seen being created.
//
// The ring and its pointers live in the test's own address space, as in
// parser_eviction_test.cpp.

#include "aql.h"
#include "parser.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kSlots = 16;
constexpr size_t kRingBytes = kSlots * aql::kPacketSize;
constexpr uint16_t kInvalidHeader =
    static_cast<uint16_t>(aql::PacketType::Invalid);

struct Ring {
    alignas(64) uint8_t bytes[kRingBytes] = {};
    alignas(uint64_t) std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr{0};
};

// Slot 0: an extended dispatch, 256 work-items in one 256-wide work-group.
// Slot 1: a classic 3-D dispatch. Its setup value (3) equals the AMD format
// byte of an extended dispatch, which is the case the decoder must not confuse.
// Slot 2: a barrier_value packet, which is not a kernel launch.
// All three headers read Invalid and all three are already consumed.
void FillConsumedRing(Ring* r) {
    aql::ExtKernelDispatchPacket ext{};
    ext.header = kInvalidHeader;
    ext.amd_format = aql::kAmdFormatExtKernelDispatch;
    ext.setup = 1;
    ext.workgroup_size_x = 256;
    ext.workgroup_size_y = 1;
    ext.workgroup_size_z = 1;
    ext.cluster_count_x = 1;
    ext.cluster_count_y = 1;
    ext.cluster_count_z = 1;
    ext.cluster_size_x = 1;
    ext.cluster_size_y = 1;
    ext.cluster_size_z = 1;
    ext.perf_hint = 0x7f;
    ext.kernel_object = 0x1000;
    memcpy(r->bytes, &ext, sizeof(ext));

    aql::KernelDispatchPacket classic{};
    classic.header = kInvalidHeader;
    classic.setup = 3;
    classic.workgroup_size_x = 2;
    classic.workgroup_size_y = 2;
    classic.workgroup_size_z = 2;
    classic.grid_size_x = 4;
    classic.grid_size_y = 6;
    classic.grid_size_z = 8;
    classic.kernel_object = 0x2000;
    memcpy(r->bytes + aql::kPacketSize, &classic, sizeof(classic));

    // hsa_amd_barrier_value_packet_t: AMD format 2 at byte 2, reserved zeros
    // at bytes 4-7, and the u32 wait condition at offset 32, where a dispatch
    // keeps kernel_object. HSA_SIGNAL_CONDITION_LT (2) is what HIP uses for
    // its stream-ordering barriers.
    uint8_t* barrier = r->bytes + 2 * aql::kPacketSize;
    memcpy(barrier, &kInvalidHeader, sizeof(kInvalidHeader));
    barrier[2] = aql::kAmdFormatBarrierValue;
    const uint32_t cond_lt = 2;
    memcpy(barrier + 32, &cond_lt, sizeof(cond_lt));

    r->wptr.store(3);
    r->rptr.store(3);
}

std::vector<hsasnoop::PacketRecord> Poll(Ring* r, bool created_during_trace) {
    std::mutex mu;
    std::vector<hsasnoop::PacketRecord> records;
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& rec) {
            std::lock_guard<std::mutex> lk(mu);
            records.push_back(rec);
        },
        {}, /*poll_us=*/500);

    hsasnoop::QueueInfo q;
    q.pid = getpid();
    q.ring_base = reinterpret_cast<uint64_t>(r->bytes);
    q.wptr_addr = reinterpret_cast<uint64_t>(&r->wptr);
    q.rptr_addr = reinterpret_cast<uint64_t>(&r->rptr);
    q.ring_size = static_cast<uint32_t>(kRingBytes);
    q.qtype = 2; // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL
    q.uid = 1;
    q.created_during_trace = created_during_trace;
    parser.AddQueue(q);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    parser.Stop();
    std::lock_guard<std::mutex> lk(mu);
    return records;
}

bool ExpectGrid(const hsasnoop::PacketRecord& rec, uint32_t x, uint32_t y,
                uint32_t z, const char* what) {
    if (rec.type == aql::PacketType::KernelDispatch && rec.grid[0] == x &&
        rec.grid[1] == y && rec.grid[2] == z)
        return true;
    std::fprintf(stderr,
                 "FAIL %s: type=%d grid=[%u,%u,%u], expected kernel dispatch "
                 "with grid=[%u,%u,%u]\n",
                 what, static_cast<int>(rec.type), rec.grid[0], rec.grid[1],
                 rec.grid[2], x, y, z);
    return false;
}

// A queue seen being created is decoded from dispatch id 0, a consumed
// dispatch is decoded with the layout its body actually has, and a consumed
// barrier is not reported as a dispatch.
bool TestNewQueueDecodesConsumedPackets() {
    Ring ring;
    FillConsumedRing(&ring);
    auto records = Poll(&ring, /*created_during_trace=*/true);
    if (records.size() != 2) {
        std::fprintf(stderr,
                     "FAIL TestNewQueueDecodesConsumedPackets: %zu records, "
                     "expected 2\n",
                     records.size());
        return false;
    }
    bool ok = ExpectGrid(records[0], 256, 1, 1, "extended dispatch");
    ok = ExpectGrid(records[1], 4, 6, 8, "classic 3-D dispatch") && ok;
    if (ok)
        std::fprintf(stderr, "PASS TestNewQueueDecodesConsumedPackets\n");
    return ok;
}

// A queue found already running reports only activity after the first poll.
bool TestExistingQueueSkipsHistory() {
    Ring ring;
    FillConsumedRing(&ring);
    auto records = Poll(&ring, /*created_during_trace=*/false);
    if (!records.empty()) {
        std::fprintf(stderr,
                     "FAIL TestExistingQueueSkipsHistory: %zu records from "
                     "packets that predate the first poll\n",
                     records.size());
        return false;
    }
    std::fprintf(stderr, "PASS TestExistingQueueSkipsHistory\n");
    return true;
}

} // namespace

int main() {
    bool ok = TestNewQueueDecodesConsumedPackets();
    ok = TestExistingQueueSkipsHistory() && ok;
    return ok ? 0 : 1;
}
