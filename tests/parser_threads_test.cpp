// parser_threads_test.cpp — several poll threads, and what the poller reports
// about its own health, without a GPU.
//
//   1. All queues of one process are polled by one thread; processes are
//      spread over the threads.
//   2. A poll thread that is held up (here by a sink that sleeps, standing in
//      for a read that blocks in the kernel) does not delay a process on
//      another thread, and the time is reported once however many of its
//      queues were overdue.
//   3. A queue that goes away while its thread is held up is reported as
//      ending with its tail unobserved; one dropped promptly is not.
//   4. Packets still in flight when a queue goes away are reported, not
//      dropped.
//   5. Time a thread is held in the last poll of a queue counts as a stall
//      even if it has no other queue, and is not mistaken for time the queue
//      went unobserved.
//   6. A queue added while its thread is in a long idle sleep is polled at
//      once and reports no stall.
//   7. When the shared watch thread blocks on one process, the poll threads
//      go back to polling their own idle queues.
//
// Cases that read a child's memory are skipped where that is not permitted.
//
// Queues of other "processes" are rings set up before a fork: the child
// inherits them at the same addresses and sleeps, and the parser reads them
// from the child.

#include "aql.h"
#include "child_mem.h"
#include "parser.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr int kSlots = 16;
constexpr size_t kRingBytes = kSlots * aql::kPacketSize;
constexpr int kPollUs = 200;

struct Ring {
    alignas(64) uint8_t bytes[kRingBytes] = {};
    alignas(uint64_t) std::atomic<uint64_t> wptr{0};
    alignas(uint64_t) std::atomic<uint64_t> rptr{0};
};

void Enqueue(Ring* r, uint64_t completion_signal = 0) {
    aql::KernelDispatchPacket p{};
    p.header = static_cast<uint16_t>(aql::PacketType::KernelDispatch);
    p.workgroup_size_x = 1;
    p.kernel_object = 0x1000;
    p.completion_signal = completion_signal;
    const uint64_t w = r->wptr.load();
    memcpy(r->bytes + (w % kSlots) * aql::kPacketSize, &p, sizeof(p));
    r->wptr.store(w + 1);
}

hsasnoop::QueueInfo Queue(Ring* r, int pid, uint64_t uid) {
    hsasnoop::QueueInfo q;
    q.pid = pid;
    q.ring_base = reinterpret_cast<uint64_t>(r->bytes);
    q.wptr_addr = reinterpret_cast<uint64_t>(&r->wptr);
    q.rptr_addr = reinterpret_cast<uint64_t>(&r->rptr);
    q.ring_size = static_cast<uint32_t>(kRingBytes);
    q.qtype = 2; // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL
    q.uid = uid;
    q.created_during_trace = true;
    return q;
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

template <typename Pred>
bool WaitFor(Pred pred, int ms) {
    for (int i = 0; i < ms; ++i) {
        if (pred())
            return true;
        SleepMs(1);
    }
    return pred();
}

// A child that sleeps until killed, with pid % 2 == parity. It shares the
// parent's rings as they were at the fork.
pid_t ForkSleeper(unsigned parity) {
    for (int i = 0; i < 64; ++i) {
        pid_t c = fork();
        if (c == 0) {
            for (;;)
                pause();
        }
        if (c < 0)
            return -1;
        if (static_cast<unsigned>(c) % 2 == parity)
            return c;
        kill(c, SIGKILL);
        waitpid(c, nullptr, 0);
    }
    return -1;
}

void Reap(pid_t c) {
    if (c > 0) {
        kill(c, SIGKILL);
        waitpid(c, nullptr, 0);
    }
}

bool TestQueuesOfAProcessShareAThread() {
    if (!ChildMemReadable("TestQueuesOfAProcessShareAThread"))
        return true;
    // One consumed dispatch in each ring, so each queue produces a record as
    // soon as it is polled.
    Ring rings[4];
    for (Ring& r : rings) {
        Enqueue(&r);
        r.rptr.store(1);
    }
    const pid_t even = ForkSleeper(0), odd = ForkSleeper(1);
    bool ok = even > 0 && odd > 0;

    std::mutex mu;
    std::map<uint64_t, std::thread::id> seen;
    {
        hsasnoop::RingParser parser(
            [&](const hsasnoop::PacketRecord& r) {
                std::lock_guard<std::mutex> lk(mu);
                seen[r.queue_uid] = std::this_thread::get_id();
            },
            {}, kPollUs, {}, -1, {}, /*poll_threads=*/2);
        ok = ok && parser.PollThreads() == 2;
        if (ok) {
            parser.AddQueue(Queue(&rings[0], even, 1));
            parser.AddQueue(Queue(&rings[1], odd, 2));
            parser.AddQueue(Queue(&rings[2], even, 3));
            parser.AddQueue(Queue(&rings[3], odd, 4));
            WaitFor(
                [&] {
                    std::lock_guard<std::mutex> lk(mu);
                    return seen.size() == 4;
                },
                2000);
        }
        parser.Stop();
    }
    Reap(even);
    Reap(odd);

    ok = ok && seen.size() == 4 && seen[1] == seen[3] && seen[2] == seen[4] &&
         seen[1] != seen[2];
    std::fprintf(stderr, "%s TestQueuesOfAProcessShareAThread (%zu records)\n",
                 ok ? "PASS" : "FAIL", seen.size());
    return ok;
}

bool TestStallIsIsolatedAndReported() {
    if (!ChildMemReadable("TestStallIsIsolatedAndReported"))
        return true;
    constexpr int kStallMs = 400;
    const unsigned mine = static_cast<unsigned>(getpid()) % 2;

    // slow: in this process; its record's sink sleeps, holding up the thread.
    // same_a/same_b: of a child on the same thread as slow, overdue with it.
    // other: of a child on the other thread.
    Ring slow, same_a, same_b, other;
    Enqueue(&other);
    other.rptr.store(1);
    const pid_t same_pid = ForkSleeper(mine);
    const pid_t other_pid = ForkSleeper(1 - mine);
    if (same_pid <= 0 || other_pid <= 0) {
        Reap(same_pid);
        Reap(other_pid);
        std::fprintf(stderr, "FAIL TestStallIsIsolatedAndReported: fork\n");
        return false;
    }

    std::atomic<bool> in_slow{false}, slow_done{false};
    std::atomic<bool> other_seen_during_stall{false};
    std::mutex mu;
    std::map<unsigned, double> stalled;
    std::map<uint64_t, bool> removed;

    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& r) {
            if (r.queue_uid == 1) {
                in_slow = true;
                SleepMs(kStallMs);
                slow_done = true;
            } else if (r.queue_uid == 4 && in_slow && !slow_done) {
                other_seen_during_stall = true;
            }
        },
        {}, kPollUs, {}, -1,
        [&](const hsasnoop::QueueInfo& q, bool tail_unobserved) {
            std::lock_guard<std::mutex> lk(mu);
            removed[q.uid] = tail_unobserved;
        },
        /*poll_threads=*/2,
        [&](unsigned thread, double seconds) {
            std::lock_guard<std::mutex> lk(mu);
            stalled[thread] += seconds;
        });

    parser.AddQueue(Queue(&slow, getpid(), 1));
    parser.AddQueue(Queue(&same_a, same_pid, 2));
    parser.AddQueue(Queue(&same_b, same_pid, 3));
    SleepMs(50); // all three polled at least once, with nothing to report

    Enqueue(&slow);
    slow.rptr.store(1);
    bool ok = WaitFor([&] { return in_slow.load(); }, 2000);

    // While that thread is held up: a queue on the other thread is served,
    // and the process behind same_a/same_b exits unseen.
    parser.AddQueue(Queue(&other, other_pid, 4));
    Reap(same_pid);
    ok = WaitFor([&] { return slow_done.load(); }, 2000) && ok;
    ok = WaitFor(
             [&] {
                 std::lock_guard<std::mutex> lk(mu);
                 return removed.size() == 2;
             },
             2000) &&
         ok;

    // Dropped promptly, on a thread that was never held up.
    Reap(other_pid);
    ok = WaitFor(
             [&] {
                 std::lock_guard<std::mutex> lk(mu);
                 return removed.size() == 3;
             },
             2000) &&
         ok;
    parser.Stop();

    std::lock_guard<std::mutex> lk(mu);
    const double lo = kStallMs * 1e-3 * 0.75, hi = kStallMs * 1e-3 * 1.75;
    auto fail = [&](const char* what) {
        std::fprintf(stderr, "FAIL TestStallIsIsolatedAndReported: %s\n", what);
        ok = false;
    };
    if (!other_seen_during_stall)
        fail("the queue on the other thread was not served during the stall");
    if (stalled[mine] < lo || stalled[mine] > hi)
        fail("stalled time of the held-up thread is not about one stall");
    if (stalled[1 - mine] > lo)
        fail("the other thread reported a stall");
    if (parser.StalledSeconds() < lo)
        fail("StalledSeconds() does not include the stall");
    if (!removed[2] || !removed[3])
        fail("queues lost during the stall were not flagged");
    if (removed[4])
        fail("a queue dropped promptly was flagged");
    if (parser.UnobservedTails() != 2)
        fail("UnobservedTails() is not 2");
    std::fprintf(stderr,
                 "%s TestStallIsIsolatedAndReported (stalled %.3f s and %.3f "
                 "s, %lu unobserved)\n",
                 ok ? "PASS" : "FAIL", stalled[mine], stalled[1 - mine],
                 parser.UnobservedTails());
    return ok;
}

// With one thread the same hold-up delays everything: the control for the
// isolation check above.
bool TestOneThreadSharesTheStall() {
    if (!ChildMemReadable("TestOneThreadSharesTheStall"))
        return true;
    Ring slow, other;
    Enqueue(&other);
    other.rptr.store(1);
    const pid_t other_pid =
        ForkSleeper(1 - static_cast<unsigned>(getpid()) % 2);
    std::atomic<bool> in_slow{false}, slow_done{false};
    std::atomic<bool> other_seen_during_stall{false}, other_seen{false};
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& r) {
            if (r.queue_uid == 1) {
                in_slow = true;
                SleepMs(300);
                slow_done = true;
            } else {
                other_seen_during_stall = !slow_done;
                other_seen = true;
            }
        },
        {}, kPollUs, {}, -1, {}, /*poll_threads=*/1);
    parser.AddQueue(Queue(&slow, getpid(), 1));
    Enqueue(&slow);
    slow.rptr.store(1);
    bool ok = other_pid > 0 && WaitFor([&] { return in_slow.load(); }, 2000);
    if (ok)
        parser.AddQueue(Queue(&other, other_pid, 2));
    ok = ok && WaitFor([&] { return other_seen.load(); }, 2000) &&
         !other_seen_during_stall && parser.PollThreads() == 1;
    parser.Stop();
    Reap(other_pid);
    std::fprintf(stderr, "%s TestOneThreadSharesTheStall\n",
                 ok ? "PASS" : "FAIL");
    return ok;
}

bool TestInFlightPacketsReportedAtDrop() {
    if (!ChildMemReadable("TestInFlightPacketsReportedAtDrop"))
        return true;
    // pending: enqueued, never consumed. unsignalled: consumed, but its
    // completion signal (amd_signal_s, value at offset 8) never fires.
    Ring pending, unsignalled;
    alignas(64) int64_t signal[8] = {0, 1};
    Enqueue(&pending);
    Enqueue(&unsignalled, reinterpret_cast<uint64_t>(signal));
    unsignalled.rptr.store(1);
    const pid_t child = ForkSleeper(0);

    std::mutex mu;
    std::map<uint64_t, std::vector<bool>> completed;
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& r) {
            std::lock_guard<std::mutex> lk(mu);
            completed[r.queue_uid].push_back(r.completed);
        },
        {}, kPollUs, {}, -1, {}, /*poll_threads=*/2);
    parser.AddQueue(Queue(&pending, child, 1));
    parser.AddQueue(Queue(&unsignalled, child, 2));
    SleepMs(100);
    bool ok = child > 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        ok = ok && completed.empty(); // both still held back
    }
    Reap(child);
    WaitFor(
        [&] {
            std::lock_guard<std::mutex> lk(mu);
            return completed.size() == 2;
        },
        2000);
    parser.Stop();
    std::lock_guard<std::mutex> lk(mu);
    ok = ok && completed[1] == std::vector<bool>{false} &&
         completed[2] == std::vector<bool>{true};
    std::fprintf(stderr, "%s TestInFlightPacketsReportedAtDrop\n",
                 ok ? "PASS" : "FAIL");
    return ok;
}

// The thread's only queue goes away, and delivering its in-flight packet
// holds the thread for a while (a slow sink, standing in for a read that
// blocks). No later poll on this thread will notice, so the time has to be
// counted there and then. The queue itself was seen on schedule up to the
// moment it went, so it is not flagged.
bool TestHoldUpAtDropIsAStallNotATail() {
    if (!ChildMemReadable("TestHoldUpAtDropIsAStallNotATail"))
        return true;
    constexpr int kHoldMs = 300;
    Ring pending;
    Enqueue(&pending);
    const pid_t child = ForkSleeper(0);
    std::atomic<int> removed{0}, flagged{0};
    std::atomic<double> sunk{0};
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord&) { SleepMs(kHoldMs); }, {}, kPollUs,
        {}, -1,
        [&](const hsasnoop::QueueInfo&, bool tail_unobserved) {
            flagged += tail_unobserved;
            ++removed;
        },
        /*poll_threads=*/1,
        [&](unsigned, double seconds) { sunk.store(sunk.load() + seconds); });
    parser.AddQueue(Queue(&pending, child, 1));
    SleepMs(100);
    Reap(child);
    bool ok = child > 0 && WaitFor([&] { return removed.load() == 1; }, 3000);
    parser.Stop();
    const double lo = kHoldMs * 1e-3 * 0.75, hi = kHoldMs * 1e-3 * 1.75;
    const double stalled = parser.StalledSeconds();
    ok = ok && stalled > lo && stalled < hi && sunk.load() > lo &&
         flagged == 0 && parser.UnobservedTails() == 0;
    std::fprintf(stderr,
                 "%s TestHoldUpAtDropIsAStallNotATail (stalled %.3f s, %d "
                 "flagged)\n",
                 ok ? "PASS" : "FAIL", stalled, flagged.load());
    return ok;
}

// With a long idle ceiling a thread sleeps for a long time between looks at
// its idle queues. A queue added meanwhile must end the sleep.
bool TestQueueAddedDuringIdleSleep() {
    constexpr int kIdleUs = 400000;
    Ring idle, late_a, late_b;
    for (Ring* r : {&late_a, &late_b}) {
        Enqueue(r);
        r->rptr.store(1);
    }
    std::mutex mu;
    std::map<uint64_t, std::chrono::steady_clock::time_point> seen;
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord& r) {
            std::lock_guard<std::mutex> lk(mu);
            seen.emplace(r.queue_uid, std::chrono::steady_clock::now());
        },
        {}, kPollUs, {}, kIdleUs, {}, /*poll_threads=*/1);
    parser.AddQueue(Queue(&idle, getpid(), 1));
    // Long enough for the idle queue's interval to pass the stall slack.
    SleepMs(1200);
    double worst_ms = 0;
    bool ok = true;
    uint64_t uid = 2;
    for (Ring* r : {&late_a, &late_b}) {
        const auto added = std::chrono::steady_clock::now();
        parser.AddQueue(Queue(r, getpid(), uid));
        ok = WaitFor(
                 [&] {
                     std::lock_guard<std::mutex> lk(mu);
                     return seen.count(uid) != 0;
                 },
                 1000) &&
             ok;
        std::lock_guard<std::mutex> lk(mu);
        if (seen.count(uid))
            worst_ms = std::max(
                worst_ms,
                std::chrono::duration<double, std::milli>(seen[uid] - added)
                    .count());
        ++uid;
        SleepMs(300);
    }
    parser.Stop();
    ok = ok && worst_ms < 40 && parser.StalledSeconds() == 0;
    std::fprintf(stderr,
                 "%s TestQueueAddedDuringIdleSleep (first record after %.1f "
                 "ms at worst, stalled %.3f s)\n",
                 ok ? "PASS" : "FAIL", worst_ms, parser.StalledSeconds());
    return ok;
}

std::atomic<int> g_block_pid{0};
std::atomic<bool> g_blocked{false};
std::atomic<uint64_t> g_watch_reads{0};
void BlockWatchRead(int pid) {
    ++g_watch_reads;
    while (pid == g_block_pid.load()) {
        g_blocked = true;
        SleepMs(1);
    }
}

// The watch thread blocks reading one process. The idle queue of a process
// on another thread must still be polled about once per idle interval, not
// once per park backstop (20 ms). When the read returns, the poll threads
// must hand their queues back to the watch thread.
bool TestBlockedWatchThread() {
    if (!ChildMemReadable("TestBlockedWatchThread"))
        return true;
    constexpr int kBlockMs = 400;
    const unsigned mine = static_cast<unsigned>(getpid()) % 2;
    Ring blocker, idle;
    const pid_t child = ForkSleeper(1 - mine);
    std::atomic<bool> seen{false};
    hsasnoop::RingParser parser(
        [&](const hsasnoop::PacketRecord&) { seen = true; }, {}, kPollUs, {},
        /*poll_idle_us=*/1000, {}, /*poll_threads=*/2);
    parser.AddQueue(Queue(&blocker, child, 1));
    parser.AddQueue(Queue(&idle, getpid(), 2));
    SleepMs(150); // both threads parked

    g_blocked = false;
    hsasnoop::RingParser::SetWatchReadHookForTest(BlockWatchRead);
    g_block_pid = child;
    bool ok = child > 0 && WaitFor([] { return g_blocked.load(); }, 1000);
    SleepMs(50); // past one backstop, however the threads were parked
    const uint64_t before = parser.QueuePolls();
    SleepMs(kBlockMs);
    const uint64_t polls = parser.QueuePolls() - before;
    Enqueue(&idle);
    idle.rptr.store(1);
    const auto t0 = std::chrono::steady_clock::now();
    ok = WaitFor([&] { return seen.load(); }, 1000) && ok;
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    g_block_pid = 0;
    SleepMs(50); // the record's queue idle again, both threads parked
    const uint64_t reads_before = g_watch_reads.load();
    SleepMs(200);
    const uint64_t watch_reads = g_watch_reads.load() - reads_before;
    hsasnoop::RingParser::SetWatchReadHookForTest(nullptr);
    parser.Stop();
    Reap(child);
    // Two queues at one poll per ms would be 800; at one per 20 ms, 40.
    // The watch thread reads both queues about once per ms when it is back.
    ok = ok && polls > 200 && watch_reads > 100 && parser.StalledSeconds() == 0;
    std::fprintf(stderr,
                 "%s TestBlockedWatchThread (%lu polls in %d ms with the "
                 "watch thread blocked, record after %.1f ms; %lu watch "
                 "reads in 200 ms after it)\n",
                 ok ? "PASS" : "FAIL", static_cast<unsigned long>(polls),
                 kBlockMs, ms, static_cast<unsigned long>(watch_reads));
    return ok;
}

} // namespace

// Timing-dependent; a loaded machine can spoil an attempt.
bool Attempts(bool (*test)()) {
    for (int i = 0; i < 3; ++i)
        if (test())
            return true;
    return false;
}

int main() {
    bool ok = Attempts(TestQueuesOfAProcessShareAThread);
    ok = Attempts(TestStallIsIsolatedAndReported) && ok;
    ok = Attempts(TestOneThreadSharesTheStall) && ok;
    ok = Attempts(TestInFlightPacketsReportedAtDrop) && ok;
    ok = Attempts(TestHoldUpAtDropIsAStallNotATail) && ok;
    ok = Attempts(TestQueueAddedDuringIdleSleep) && ok;
    ok = Attempts(TestBlockedWatchThread) && ok;
    return ok ? 0 : 1;
}
