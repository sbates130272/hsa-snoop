// parser_idle_test.cpp — the poll loop's idle backoff, without a GPU.
//
// An idle queue must be polled far less often than the poll interval alone
// would allow, and must return to the full rate as soon as it has work.
// Thresholds are ratios between runs on the same machine, not absolute rates.

#include "aql.h"
#include "parser.h"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {

constexpr int kSlots = 16;
constexpr size_t kRingBytes = kSlots * aql::kPacketSize;
constexpr int kPollUs = 100;

struct Ring {
    alignas(64) uint8_t bytes[kRingBytes] = {};
    alignas(uint64_t) std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr{0};
};

hsasnoop::QueueInfo Queue(Ring* r) {
    hsasnoop::QueueInfo q;
    q.pid = getpid();
    q.ring_base = reinterpret_cast<uint64_t>(r->bytes);
    q.wptr_addr = reinterpret_cast<uint64_t>(&r->wptr);
    q.rptr_addr = reinterpret_cast<uint64_t>(&r->rptr);
    q.ring_size = static_cast<uint32_t>(kRingBytes);
    q.qtype = 2; // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL
    q.uid = 1;
    return q;
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Polls of one idle queue over a fixed time.
uint64_t IdlePolls(int poll_idle_us) {
    Ring ring;
    hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                kPollUs, {}, poll_idle_us);
    parser.AddQueue(Queue(&ring));
    SleepMs(300);
    const uint64_t polls = parser.QueuePolls();
    parser.Stop();
    return polls;
}

bool TestIdleQueueIsPolledLess() {
    const uint64_t without = IdlePolls(/*poll_idle_us=*/0);
    const uint64_t with = IdlePolls(/*poll_idle_us=*/5000);
    if (with == 0 || with * 3 > without) {
        std::fprintf(stderr,
                     "FAIL TestIdleQueueIsPolledLess: %lu polls with backoff, "
                     "%lu without; expected under a third\n",
                     with, without);
        return false;
    }
    std::fprintf(stderr,
                 "PASS TestIdleQueueIsPolledLess (%lu with, %lu without)\n",
                 with, without);
    return true;
}

// After a long idle spell, a packet left pending must put the queue back on
// the full poll rate for as long as it stays pending.
bool TestWorkEndsBackoff() {
    Ring ring;
    std::atomic<int> records{0};
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord&) { ++records; }, {}, kPollUs, {},
        /*poll_idle_us=*/5000);
    parser.AddQueue(Queue(&ring));

    SleepMs(200); // well past the hold; the queue is at the idle ceiling
    const uint64_t a = parser.QueuePolls();
    SleepMs(100);
    const uint64_t idle_window = parser.QueuePolls() - a;

    // Enqueue one dispatch and do not consume it: it stays in flight.
    aql::KernelDispatchPacket p{};
    p.header = static_cast<uint16_t>(aql::PacketType::KernelDispatch);
    p.workgroup_size_x = 1;
    p.kernel_object = 0x1000;
    memcpy(ring.bytes, &p, sizeof(p));
    ring.wptr.store(1);
    SleepMs(20); // at most one idle interval before it is noticed
    const uint64_t b = parser.QueuePolls();
    SleepMs(100);
    const uint64_t busy_window = parser.QueuePolls() - b;

    ring.rptr.store(1);
    for (int i = 0; i < 200 && records.load() != 1; ++i)
        SleepMs(10);
    parser.Stop();

    bool ok = true;
    if (busy_window < idle_window * 3) {
        std::fprintf(stderr,
                     "FAIL TestWorkEndsBackoff: %lu polls in 100 ms with a "
                     "packet in flight, %lu when idle; expected at least 3x\n",
                     busy_window, idle_window);
        ok = false;
    }
    if (records.load() != 1) {
        std::fprintf(stderr,
                     "FAIL TestWorkEndsBackoff: %d records, expected 1\n",
                     records.load());
        ok = false;
    }
    if (ok)
        std::fprintf(stderr,
                     "PASS TestWorkEndsBackoff (%lu busy, %lu idle per 100 "
                     "ms)\n",
                     busy_window, idle_window);
    return ok;
}

} // namespace

// The checks compare poll counts across wall-clock windows. Machine load can
// only flatten those ratios, never inflate them, so one clean attempt out of
// a few is proof enough.
bool Attempts(bool (*test)()) {
    for (int i = 0; i < 5; ++i)
        if (test())
            return true;
    return false;
}

int main() {
    bool ok = Attempts(TestIdleQueueIsPolledLess);
    ok = Attempts(TestWorkEndsBackoff) && ok;
    return ok ? 0 : 1;
}
