// parser.h - Per-queue ring poller. Watches the write/read pointers in the
// target process and decodes packets as they are enqueued and queue-consumed.
// Handles two ring formats: fixed 64-byte AQL compute packets (kernel
// dispatches) and variable-length SDMA microcode packets (DMA copies).
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ksym.h"
#include "model.h"

namespace hsasnoop {

class RingParser {
  public:
    using Sink = std::function<void(const PacketRecord&)>;
    using SdmaSink = std::function<void(const SdmaRecord&)>;
    // (queue_uid, n, reason): n AQL packets on that queue went by without
    // being decoded. reason is a static string:
    //   "overwritten" - the producer wrapped the ring before they were read
    //   "consumed"    - already consumed, and the slot no longer holds a
    //                   decodable packet
    //   "unreadable"  - already consumed, and reading the slot failed
    using MissSink = std::function<void(uint64_t, uint64_t, const char*)>;
    // A queue the parser has stopped polling for good: its process exited, it
    // was destroyed, or it was evicted as stale. Called on the queue's poll
    // thread after the queue's last record, with no parser lock held. Not
    // called for queues still being polled at Stop().
    //
    // tail_unobserved: the last successful pointer read was longer before the
    // queue went away than its poll interval allows (or there never was one),
    // so what it did in between was not seen and is in none of the counts.
    using RemoveSink =
        std::function<void(const QueueInfo&, bool tail_unobserved)>;
    // (thread, seconds): poll thread `thread` has just got to a queue that
    // much later than the queue was due. Time during which several of a
    // thread's queues were overdue is reported once. Called on that thread.
    using StallSink = std::function<void(unsigned, double)>;

    // poll_us: sleep between polls (microseconds). Lower catches faster
    // kernels/copies. sdma_sink may be empty if SDMA capture is not wanted.
    //
    // poll_idle_us: the longest a queue that has stopped moving goes between
    // polls. A queue is polled every poll_us while it has activity, and for a
    // short hold after; then its interval grows up to this. -1 picks the
    // default, the larger of poll_us and 1000; 0 turns the backoff off.
    //
    // poll_threads: number of poll threads. All queues of one process are
    // polled by the same thread, so a read that blocks holds up only the
    // processes sharing that thread. (While their queues are all idle the
    // threads leave them to one shared watch thread; if a read blocks that
    // one, each thread goes back to polling its own within a few idle
    // intervals.) Every sink is then called from several
    // threads at once, though never concurrently for queues of one process.
    // 0 picks DefaultPollThreads(); 1 is a single thread and nothing else.
    RingParser(Sink sink, SdmaSink sdma_sink = {}, int poll_us = 20,
               MissSink miss_sink = {}, int poll_idle_us = -1,
               RemoveSink remove_sink = {}, int poll_threads = 0,
               StallSink stall_sink = {});
    ~RingParser();

    static constexpr int kMaxPollThreads = 64;
    static int DefaultPollThreads();

    // Begin polling a newly discovered queue (thread-safe). AQL and SDMA queues
    // are both accepted; the ring format is chosen from q.is_sdma().
    void AddQueue(const QueueInfo& q);
    void Stop();

    unsigned PollThreads() const { return workers_.size(); }
    // Number of times any queue's pointers have been read. For tests.
    uint64_t QueuePolls() const { return queue_polls_.load(); }
    // Totals of what StallSink and RemoveSink report.
    double StalledSeconds() const;
    // For tests: called on the watch thread before it reads the pointers of
    // a queue of `pid`, to stand in for a read that blocks in the kernel.
    static void SetWatchReadHookForTest(void (*hook)(int pid));
    uint64_t UnobservedTails() const { return unobserved_tails_.load(); }

  private:
    struct QueueState {
        QueueInfo info;
        std::unique_ptr<KernelSymbolResolver> ksym; // AQL only
        uint64_t next_id = 0; // AQL: next dispatch id; SDMA: next ring dword
        uint64_t last_rptr = 0;
        bool primed = false;
        uint64_t dropped = 0;
        // Totals for the exit summary. AQL: packets; SDMA: packets decoded.
        uint64_t decoded = 0;
        uint64_t missed_overwritten = 0;
        uint64_t missed_consumed = 0;
        uint64_t missed_unreadable = 0;
        // Ring position at the first poll of a queue that already existed:
        // everything before it predates the poller and was not looked at.
        uint64_t skipped_at_start = 0;
        // Write pointer at the last successful poll, in ring units. Whatever
        // lies between next_id and it when polling stops was never decoded.
        uint64_t last_wptr = 0;

        // Pointer reads: true once process_vm_readv has failed and
        // /proc/<pid>/mem worked, so the failing call is not repeated.
        bool ptr_via_mem = false;
        // Raw pointer values at the previous poll, to tell an idle queue
        // from one that moved.
        bool ptrs_seen = false;
        bool moved = false;
        uint64_t seen_wptr = 0, seen_rptr = 0;
        // Idle backoff and liveness scheduling (CLOCK_MONOTONIC_RAW seconds).
        double last_active_ts = 0;
        double next_poll_ts = 0;
        double next_alive_ts = 0;
        // Poller health. last_poll_ts is when the latest poll started (or
        // when the queue was added) and poll_interval how long after it the
        // next one was meant to start; the ok_ pair is the same for the
        // latest poll whose pointer read succeeded.
        double last_poll_ts = 0;
        double poll_interval = 0;
        double last_ok_ts = 0;
        double ok_interval = 0;
        uint64_t ok_polls = 0;
        double max_gap = 0;
        // When the queue was found to have gone: the end of the failed read,
        // or of the first of a run of them.
        double gone_ts = 0;
        double fail_since_ts = 0;
        double next_map_check_ts = 0;
        bool tail_unobserved = false;
        double unobserved_sec = 0;
        std::map<uint64_t, PacketRecord> inflight;    // AQL: id -> record
        uint64_t sdma_seq = 0;                        // SDMA packet counter
        std::map<uint64_t, SdmaRecord> sdma_inflight; // SDMA: end-dword -> rec
        // Cache: page-aligned VA -> host-backed? (used for copy direction).
        std::unordered_map<uint64_t, bool> page_host;
        // Consecutive pointer-read failures. On WSL2/bpftrace --all mode, PID
        // reuse can cause a queue record to outlive the process that created
        // it; the ring VA is then invalid in the new process. Evict after
        // threshold.
        int read_fail_count = 0;
        // Set once process_vm_readv has failed on the ring and /proc/<pid>/mem
        // has succeeded, so later reads skip the failing syscall.
        bool ring_via_mem = false;
        bool evict = false;
    };

    // One poll thread and the queues it owns.
    struct Worker {
        unsigned index = 0;
        std::mutex mu; // guards queues
        std::condition_variable cv;
        std::vector<std::unique_ptr<QueueState>> queues;
        std::thread thread;
        // Set while the thread sleeps with all of its queues idle and
        // leaves them to the watch thread; kick ends that. park_gen counts
        // the times it has parked. Guarded by mu.
        bool parked = false;
        bool kick = false;
        uint64_t park_gen = 0;
        std::atomic<double> stalled_sec{0};
        // Touched by the worker's own thread only.
        double stall_counted_until = 0;
        double stall_unlogged = 0;
        double stall_log_ts = 0;
    };

    void Run(Worker* w);
    void Park(Worker* w);
    void Watch();
    void Watchdog();
    void ArmWatchdog(int64_t ns);
    bool CountStall(Worker* w, double from, double to);
    void LogStall(Worker* w, const QueueState& qs, double now, bool late_start,
                  double sec);
    void NoteStall(Worker* w, QueueState* qs, double now);
    void NoteHeld(Worker* w, QueueState* qs, double start);
    void NoteDropped(QueueState* qs);
    bool ReadPtrs(QueueState* qs, uint64_t* wptr, uint64_t* rptr);
    static bool QueueGone(QueueState* qs, double now, bool last_chance);
    void PtrReadFailed(QueueState* qs);
    static void PrintSummary(const QueueState& qs);

    // AQL path.
    void PollQueue(QueueState* qs, double now);
    bool DecodeSlot(QueueState* qs, uint64_t id, uint64_t rptr,
                    PacketRecord* rec, bool* read_failed);

    bool ReadRing(QueueState* qs, uint64_t va, void* out, size_t len);

    // SDMA path.
    void PollSdmaQueue(QueueState* qs, double now);
    bool ReadRingDwords(QueueState* qs, uint64_t start_dw, uint32_t n,
                        uint32_t* out);
    CopyDir ClassifyDir(QueueState* qs, uint64_t src, uint64_t dst);
    bool IsHostVA(QueueState* qs, uint64_t va);

    Sink sink_;
    SdmaSink sdma_sink_;
    MissSink miss_sink_;
    RemoveSink remove_sink_;
    StallSink stall_sink_;
    int poll_us_;
    int poll_idle_us_;
    std::atomic<uint64_t> queue_polls_{0};
    std::atomic<uint64_t> unobserved_tails_{0};
    // Fixed after construction; a queue goes to workers_[pid % size].
    std::vector<std::unique_ptr<Worker>> workers_;
    // With several poll threads, one more thread looks at the pointers of
    // every queue whose own thread is parked, so that idle queues cost one
    // wake-up per idle interval in total, not one per poll thread.
    bool watch_ = false; // fixed before any thread starts
    std::thread watch_thread_;
    std::mutex watch_mu_;
    std::condition_variable watch_cv_;
    int parked_workers_ = 0; // guarded by watch_mu_
    // Supervises the watch thread: a timer the watch thread pushes back on
    // every pass, and a thread that sleeps until it goes off.
    int watchdog_fd_ = -1;
    std::thread watchdog_thread_;
    std::atomic<bool> watch_held_up_{false};
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
