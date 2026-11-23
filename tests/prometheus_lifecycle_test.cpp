// prometheus_lifecycle_test.cpp — queue and series lifecycle of the
// Prometheus exporter, without a GPU:
//
//   1. hsa_active_queues / hsa_active_sdma_queues go down when a queue is
//      unregistered, and its hsa_queue_info series goes with it.
//   2. The series of a pid survive its last queue for the retention period,
//      with their final values, and are removed after it. Per-GPU totals and
//      per-kernel series are not removed.
//   3. A pid that comes back before the period ends keeps its series; one
//      that comes back after gets new ones.
//   4. With retention 0 nothing is ever removed.
//   5. The series of a process known only from AIS records (it has no queue
//      here) stay for as long as the process does.
//   6. Registering, recording, unregistering, expiring and scraping from
//      several threads at once does not crash.

#include "prometheus_exporter.h"

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using hsasnoop::PrometheusExporter;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGpu = 4242;

std::unique_ptr<PrometheusExporter> MakeExporter(double retain_sec) {
    hsasnoop::PrometheusExporterOptions opt;
    opt.host_label = "test";
    opt.rocm_version = "test";
    opt.series_retain_sec = retain_sec;
    // The HTTP endpoint is not used, but the exporter always opens one.
    for (int i = 0; i < 50; ++i) {
        const uint16_t port =
            static_cast<uint16_t>(20000 + (getpid() * 7 + i * 131) % 20000);
        try {
            return std::make_unique<PrometheusExporter>(port, 10.0, "kprobe",
                                                        opt);
        } catch (const std::exception&) {
        }
    }
    return nullptr;
}

hsasnoop::QueueInfo Queue(uint64_t uid, int pid, bool sdma = false) {
    hsasnoop::QueueInfo q;
    q.uid = uid;
    q.pid = pid;
    q.comm = "work";
    q.gpu_id = kGpu;
    q.qtype = sdma ? 1 : 2;
    q.sdma_version = sdma::Version::V6;
    q.ring_base = 0x10000 * uid;
    q.ring_size = 4096;
    return q;
}

hsasnoop::PacketRecord Dispatch(uint64_t uid) {
    hsasnoop::PacketRecord r;
    r.queue_uid = uid;
    r.type = aql::PacketType::KernelDispatch;
    r.kernel_name = "kern";
    r.submit_ts = 1.0;
    r.complete_ts = 1.001;
    r.completed = true;
    return r;
}

hsasnoop::SdmaRecord Copy(uint64_t uid) {
    hsasnoop::SdmaRecord r;
    r.queue_uid = uid;
    r.opcode = sdma::OP_COPY;
    r.sdma_version = sdma::Version::V6;
    r.bytes = 4096;
    r.dir = hsasnoop::CopyDir::HostToDevice;
    return r;
}

// Sum of the values of every sample of `metric` whose label set contains
// `label` (empty = any). -1 if there is no such sample.
double Value(const std::string& snapshot, const std::string& metric,
             const std::string& label = "") {
    std::istringstream in(snapshot);
    std::string line;
    double sum = 0;
    bool found = false;
    while (std::getline(in, line)) {
        if (line.compare(0, metric.size(), metric) != 0)
            continue;
        const char next = line.size() > metric.size() ? line[metric.size()] : 0;
        if (next != '{' && next != ' ')
            continue;
        if (!label.empty() && line.find(label) == std::string::npos)
            continue;
        sum += atof(line.substr(line.rfind(' ') + 1).c_str());
        found = true;
    }
    return found ? sum : -1;
}

int Count(const std::string& snapshot, const std::string& needle) {
    int n = 0;
    std::istringstream in(snapshot);
    std::string line;
    while (std::getline(in, line))
        if (line[0] != '#' && line.find(needle) != std::string::npos)
            ++n;
    return n;
}

int g_failures = 0;

void Expect(bool cond, const char* test, const char* what) {
    if (cond)
        return;
    std::fprintf(stderr, "FAIL %s: %s\n", test, what);
    ++g_failures;
}

#define EXPECT(cond) Expect((cond), __func__, #cond)

void TestActiveGaugesAndQueueInfo() {
    auto e = MakeExporter(300);
    EXPECT(e != nullptr);
    if (!e)
        return;
    e->RegisterQueue(Queue(1, 100));
    e->RegisterQueue(Queue(2, 100));
    e->RegisterQueue(Queue(3, 100, /*sdma=*/true));
    std::string s = e->Snapshot();
    EXPECT(Value(s, "hsa_active_queues") == 2);
    EXPECT(Value(s, "hsa_active_sdma_queues") == 1);
    EXPECT(Count(s, "hsa_queue_info{") == 3);

    e->UnregisterQueue(1);
    e->UnregisterQueue(1);  // a repeat is ignored
    e->UnregisterQueue(99); // so is a uid never registered
    s = e->Snapshot();
    EXPECT(Value(s, "hsa_active_queues") == 1);
    EXPECT(Value(s, "hsa_active_sdma_queues") == 1);
    EXPECT(Count(s, "hsa_queue_info{") == 2);
    EXPECT(Count(s, "ring_base=\"0x10000\"") == 0);

    e->UnregisterQueue(2);
    e->UnregisterQueue(3);
    s = e->Snapshot();
    // The gauges stay, at 0; a dashboard variable is built from them.
    EXPECT(Value(s, "hsa_active_queues") == 0);
    EXPECT(Value(s, "hsa_active_sdma_queues") == 0);
    EXPECT(Count(s, "hsa_queue_info{") == 0);

    // Two queues that share one hsa_queue_info label set: a pid reused with
    // the same ring address before the old queue was dropped.
    hsasnoop::QueueInfo a = Queue(10, 200), b = Queue(11, 200);
    b.ring_base = a.ring_base;
    e->RegisterQueue(a);
    e->RegisterQueue(b);
    EXPECT(Count(e->Snapshot(), "hsa_queue_info{") == 1);
    e->UnregisterQueue(10);
    EXPECT(Count(e->Snapshot(), "hsa_queue_info{") == 1);
    e->UnregisterQueue(11);
    EXPECT(Count(e->Snapshot(), "hsa_queue_info{") == 0);
}

void TestRetention() {
    auto e = MakeExporter(300);
    if (!e)
        return;
    const auto retain = std::chrono::seconds(300);
    e->RegisterQueue(Queue(1, 100));
    e->RegisterQueue(Queue(2, 100, /*sdma=*/true));
    e->RegisterQueue(Queue(3, 101));
    for (int i = 0; i < 5; ++i)
        e->Add(Dispatch(1));
    for (int i = 0; i < 3; ++i)
        e->Add(Copy(2));
    e->Add(Dispatch(3));
    e->AddMissed(1, 7, "overwritten");

    // Live queues: nothing expires however late it is.
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) == 0);

    e->UnregisterQueue(1);
    // One queue of pid 100 is left.
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) == 0);
    e->UnregisterQueue(2);

    // Inside the retention period the final values are still exported.
    EXPECT(e->ExpireSeries(Clock::now() + retain / 2) == 0);
    std::string s = e->Snapshot();
    EXPECT(Value(s, "hsa_kernel_launches_total", "pid=\"100\"") == 5);
    EXPECT(Value(s, "hsa_sdma_copies_total", "pid=\"100\"") == 3);
    EXPECT(Value(s, "hsa_dispatches_missed_total", "pid=\"100\"") == 7);

    // After it they are gone; pid 101 is untouched.
    EXPECT(e->ExpireSeries(Clock::now() + retain + std::chrono::seconds(5)) >
           0);
    s = e->Snapshot();
    EXPECT(Count(s, "pid=\"100\"") == 0);
    EXPECT(Value(s, "hsa_kernel_launches_total", "pid=\"101\"") == 1);
    // What has no pid label stays, with everything counted.
    EXPECT(Value(s, "hsa_gpu_kernel_launches_total") == 6);
    EXPECT(Value(s, "hsa_gpu_dispatches_missed_total") == 7);
    EXPECT(Value(s, "hsa_gpu_sdma_copies_total") == 3);
    EXPECT(Value(s, "hsa_gpu_sdma_bytes_total") == 3 * 4096);
    EXPECT(Value(s, "hsa_gpu_sdma_packets_total") == 3);
    EXPECT(Value(s, "hsa_kernel_duration_seconds_count") == 6);
    // A second pass has nothing left to remove.
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) == 0);
}

void TestPidComesBack() {
    auto e = MakeExporter(300);
    if (!e)
        return;
    const auto retain = std::chrono::seconds(300);
    e->RegisterQueue(Queue(1, 100));
    e->Add(Dispatch(1));
    e->Add(Dispatch(1));
    e->UnregisterQueue(1);

    // Back before the period ends: the removal is cancelled and the counter
    // carries on.
    e->RegisterQueue(Queue(2, 100));
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) == 0);
    e->Add(Dispatch(2));
    EXPECT(Value(e->Snapshot(), "hsa_kernel_launches_total", "pid=\"100\"") ==
           3);

    e->UnregisterQueue(2);
    EXPECT(e->ExpireSeries(Clock::now() + 2 * retain) > 0);
    EXPECT(Count(e->Snapshot(), "pid=\"100\"") == 0);

    // Back after: new series, from 0, and they expire in their turn.
    e->RegisterQueue(Queue(3, 100));
    e->Add(Dispatch(3));
    std::string s = e->Snapshot();
    EXPECT(Value(s, "hsa_kernel_launches_total", "pid=\"100\"") == 1);
    EXPECT(Value(s, "hsa_gpu_kernel_launches_total") == 4);
    e->UnregisterQueue(3);
    EXPECT(e->ExpireSeries(Clock::now() + 2 * retain) > 0);
    EXPECT(Count(e->Snapshot(), "pid=\"100\"") == 0);
}

void TestRetainForever() {
    auto e = MakeExporter(0);
    if (!e)
        return;
    e->RegisterQueue(Queue(1, 100));
    e->Add(Dispatch(1));
    e->UnregisterQueue(1);
    EXPECT(e->ExpireSeries(Clock::now() + std::chrono::hours(1000)) == 0);
    const std::string s = e->Snapshot();
    EXPECT(Value(s, "hsa_kernel_launches_total", "pid=\"100\"") == 1);
    EXPECT(Value(s, "hsa_active_queues") == 0);
}

hsasnoop::AisRecord AisWrite(int pid) {
    hsasnoop::AisRecord r;
    r.pid = pid;
    r.comm = "ckpt";
    r.op = hsasnoop::AisOp::Write;
    r.gpu_id = kGpu;
    r.size_req = r.size_copied = 4096;
    r.submit_ts = 1.0;
    r.complete_ts = 1.001;
    r.completed = true;
    return r;
}

// A process that does AIS I/O less often than the retention period: were
// its counter removed between two operations, it would come back at 1 each
// time and no rate() or increase() would ever see an operation.
void TestAisOnlyProcess() {
    auto e = MakeExporter(300);
    if (!e)
        return;
    const auto retain = std::chrono::seconds(300);
    const std::string me = "pid=\"" + std::to_string(getpid()) + "\"";
    e->Add(AisWrite(getpid()));
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) == 0);
    e->Add(AisWrite(getpid()));
    EXPECT(Value(e->Snapshot(), "ais_tx_ops_total", me) == 2);

    // One that has exited goes after the period, as a queue's owner does.
    pid_t child = fork();
    if (child == 0)
        _exit(0);
    EXPECT(child > 0);
    if (child <= 0)
        return;
    waitpid(child, nullptr, 0);
    const std::string gone = "pid=\"" + std::to_string(child) + "\"";
    e->Add(AisWrite(child));
    EXPECT(Value(e->Snapshot(), "ais_tx_ops_total", gone) == 1);
    EXPECT(e->ExpireSeries(Clock::now() + retain / 2) == 0);
    EXPECT(e->ExpireSeries(Clock::now() + 2 * retain) == 1);
    const std::string s = e->Snapshot();
    EXPECT(Count(s, gone) == 0);
    EXPECT(Value(s, "ais_tx_ops_total", me) == 2);
}

void TestConcurrent() {
    auto e = MakeExporter(300);
    if (!e)
        return;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> next_uid{1};
    std::vector<std::thread> threads;
    for (int t = 0; t < 3; ++t)
        threads.emplace_back([&, t] {
            while (!stop) {
                const uint64_t uid = next_uid++;
                const int pid = 1000 + static_cast<int>(uid % 7);
                e->RegisterQueue(Queue(uid, pid, t == 2));
                for (int i = 0; i < 20; ++i) {
                    if (t == 2)
                        e->Add(Copy(uid));
                    else
                        e->Add(Dispatch(uid));
                }
                e->AddMissed(uid, 1, "consumed");
                e->UnregisterQueue(uid);
            }
        });
    threads.emplace_back([&] {
        while (!stop)
            e->ExpireSeries(Clock::now() + std::chrono::hours(1));
    });
    size_t bytes = 0;
    threads.emplace_back([&] {
        while (!stop)
            bytes += e->Snapshot().size();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop = true;
    for (auto& th : threads)
        th.join();

    EXPECT(bytes > 0);
    EXPECT(e->ExpireSeries(Clock::now() + std::chrono::hours(1)) + 1 > 0);
    const std::string s = e->Snapshot();
    EXPECT(Count(s, "pid=\"1") == 0);
    EXPECT(Value(s, "hsa_active_queues") == 0);
    EXPECT(Value(s, "hsa_active_sdma_queues") == 0);
    EXPECT(Count(s, "hsa_queue_info{") == 0);
    EXPECT(Value(s, "hsa_gpu_kernel_launches_total") > 0);
}

// The poller-health counters carry no pid label, so they outlive the
// processes they are about.
void TestPollerHealth() {
    auto e = MakeExporter(300);
    if (!e)
        return;
    const auto retain = std::chrono::seconds(300);
    e->AddPollerStall(0, 0);
    e->AddPollerStall(1, 0);
    e->RegisterQueue(Queue(1, 100));
    e->RegisterQueue(Queue(2, 100, /*sdma=*/true));
    e->RegisterQueue(Queue(3, 101));
    std::string s = e->Snapshot();
    EXPECT(Count(s, "hsa_poller_stalled_seconds_total{") == 2);
    EXPECT(Value(s, "hsa_poller_stalled_seconds_total") == 0);
    EXPECT(Value(s, "hsa_queues_unobserved_tail_total", "kind=\"aql\"") == 0);
    EXPECT(Value(s, "hsa_queues_unobserved_tail_total", "kind=\"sdma\"") == 0);

    e->AddPollerStall(1, 1.5);
    e->AddPollerStall(1, 0.25);
    e->UnregisterQueue(1, /*tail_unobserved=*/true);
    e->UnregisterQueue(1, /*tail_unobserved=*/true); // already gone
    e->UnregisterQueue(2, /*tail_unobserved=*/true);
    e->UnregisterQueue(3);
    EXPECT(e->ExpireSeries(Clock::now() + 10 * retain) > 0);
    s = e->Snapshot();
    EXPECT(Value(s, "hsa_poller_stalled_seconds_total", "thread=\"0\"") == 0);
    EXPECT(Value(s, "hsa_poller_stalled_seconds_total", "thread=\"1\"") ==
           1.75);
    EXPECT(Value(s, "hsa_queues_unobserved_tail_total", "kind=\"aql\"") == 1);
    EXPECT(Value(s, "hsa_queues_unobserved_tail_total", "kind=\"sdma\"") == 1);
    EXPECT(Count(s, "hsa_queues_unobserved_tail_total{") == 2);
    EXPECT(Count(s, "pid=\"10") == 0);
}

} // namespace

int main() {
    TestActiveGaugesAndQueueInfo();
    TestRetention();
    TestPidComesBack();
    TestRetainForever();
    TestAisOnlyProcess();
    TestConcurrent();
    TestPollerHealth();
    if (g_failures == 0)
        std::fprintf(stderr, "PASS prometheus-lifecycle-test\n");
    return g_failures ? 1 : 0;
}
