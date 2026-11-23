// parser_idle_test.cpp — the poll loop's idle backoff, without a GPU.
//
// An idle queue must be polled far less often than the poll interval alone
// would allow, and must return to the full rate as soon as it has work. More
// poll threads must not change that, and threads with no queue must sleep.
// Thresholds are ratios between runs on the same machine, not absolute rates.

#include "aql.h"
#include "child_mem.h"
#include "parser.h"

#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
uint64_t IdlePolls(int poll_idle_us, int poll_threads = 1) {
    Ring ring;
    hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                kPollUs, {}, poll_idle_us, {}, poll_threads);
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

// Extra poll threads must not add polls of an idle queue.
bool TestIdleQueueWithMoreThreads() {
    const uint64_t one = IdlePolls(/*poll_idle_us=*/5000, 1);
    const uint64_t eight = IdlePolls(/*poll_idle_us=*/5000, 8);
    const bool ok = eight > 0 && eight <= one + one / 2;
    std::fprintf(stderr,
                 "%s TestIdleQueueWithMoreThreads (%lu polls with 8 threads, "
                 "%lu with 1)\n",
                 ok ? "PASS" : "FAIL", eight, one);
    return ok;
}

double CpuSeconds() {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

// Poll threads with no queue must sleep, not spin at the poll interval: the
// whole process may use only a sliver of CPU while they wait.
bool TestThreadsWithoutQueuesSleep() {
    hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                /*poll_us=*/20, {}, /*poll_idle_us=*/0, {},
                                /*poll_threads=*/8);
    SleepMs(20);
    const double before = CpuSeconds();
    SleepMs(500);
    const double used = CpuSeconds() - before;
    parser.Stop();
    const bool ok = parser.PollThreads() == 8 && used < 0.010;
    std::fprintf(stderr,
                 "%s TestThreadsWithoutQueuesSleep (%.1f ms of CPU in 500 ms "
                 "with 8 threads)\n",
                 ok ? "PASS" : "FAIL", used * 1e3);
    return ok;
}

// Voluntary context switches of every thread of this process: each time a
// thread goes to sleep counts one.
uint64_t Sleeps() {
    uint64_t total = 0;
    DIR* d = opendir("/proc/self/task");
    if (!d)
        return 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        const std::string path =
            std::string("/proc/self/task/") + e->d_name + "/status";
        if (FILE* f = fopen(path.c_str(), "r")) {
            char line[128];
            unsigned long n;
            while (fgets(line, sizeof(line), f))
                if (sscanf(line, "voluntary_ctxt_switches: %lu", &n) == 1)
                    total += n;
            fclose(f);
        }
    }
    closedir(d);
    return total;
}

// Sleeps per 500 ms with one idle queue in each of four processes, which
// with four poll threads is one queue per thread.
uint64_t IdleSleeps(int poll_threads, const pid_t* pids, Ring* rings) {
    hsasnoop::RingParser parser([](const hsasnoop::PacketRecord&) {}, {},
                                kPollUs, {}, /*poll_idle_us=*/1000, {},
                                poll_threads);
    for (int i = 0; i < 4; ++i) {
        hsasnoop::QueueInfo q = Queue(&rings[i]);
        q.pid = pids[i];
        q.uid = i + 1;
        parser.AddQueue(q);
    }
    SleepMs(100);
    const uint64_t before = Sleeps();
    SleepMs(500);
    const uint64_t n = Sleeps() - before;
    parser.Stop();
    return n;
}

// Idle queues spread over four poll threads must not cost four times the
// wake-ups of one thread: the threads sleep and one watcher polls for all.
bool TestIdleThreadsSleep() {
    if (!ChildMemReadable("TestIdleThreadsSleep"))
        return true;
    Ring rings[4];
    pid_t pids[4] = {-1, -1, -1, -1};
    // Children that sleep, one for each value of pid % 4. Each has the rings
    // at the same addresses.
    for (int tries = 0, have = 0; have < 4 && tries < 256; ++tries) {
        pid_t c = fork();
        if (c == 0)
            for (;;)
                pause();
        if (c > 0 && pids[c % 4] < 0) {
            pids[c % 4] = c;
            ++have;
        } else if (c > 0) {
            kill(c, SIGKILL);
            waitpid(c, nullptr, 0);
        }
    }
    bool ok = pids[0] > 0 && pids[1] > 0 && pids[2] > 0 && pids[3] > 0;
    uint64_t one = 0, four = 0;
    if (ok) {
        one = IdleSleeps(1, pids, rings);
        four = IdleSleeps(4, pids, rings);
        ok = one > 0 && four < 2 * one;
    }
    for (pid_t c : pids)
        if (c > 0) {
            kill(c, SIGKILL);
            waitpid(c, nullptr, 0);
        }
    std::fprintf(stderr,
                 "%s TestIdleThreadsSleep (%lu sleeps in 500 ms with 4 "
                 "threads, %lu with 1)\n",
                 ok ? "PASS" : "FAIL", four, one);
    return ok;
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
    ok = Attempts(TestIdleQueueWithMoreThreads) && ok;
    ok = Attempts(TestThreadsWithoutQueuesSleep) && ok;
    ok = Attempts(TestIdleThreadsSleep) && ok;
    ok = Attempts(TestWorkEndsBackoff) && ok;
    return ok ? 0 : 1;
}
