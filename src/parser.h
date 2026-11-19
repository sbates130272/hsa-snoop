// parser.h - Per-queue ring poller. Watches the write/read pointers in the
// target process and decodes packets as they are enqueued and queue-consumed.
// Handles two ring formats: fixed 64-byte AQL compute packets (kernel
// dispatches) and variable-length SDMA microcode packets (DMA copies).
#pragma once
#include <atomic>
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

    // poll_us: sleep between polls (microseconds). Lower catches faster
    // kernels/copies. sdma_sink may be empty if SDMA capture is not wanted.
    RingParser(Sink sink, SdmaSink sdma_sink = {}, int poll_us = 20,
               MissSink miss_sink = {});
    ~RingParser();

    // Begin polling a newly discovered queue (thread-safe). AQL and SDMA queues
    // are both accepted; the ring format is chosen from q.is_sdma().
    void AddQueue(const QueueInfo& q);
    void Stop();

  private:
    struct QueueState {
        QueueInfo info;
        std::unique_ptr<KernelSymbolResolver> ksym; // AQL only
        uint64_t next_id = 0; // AQL: next dispatch id; SDMA: next ring dword
        uint64_t last_rptr = 0;
        bool primed = false;
        uint64_t dropped = 0;
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
        bool evict = false;
    };

    void Run();

    // AQL path.
    void PollQueue(QueueState* qs, double now);
    bool DecodeSlot(QueueState* qs, uint64_t id, uint64_t rptr,
                    PacketRecord* rec, bool* read_failed);

    // SDMA path.
    void PollSdmaQueue(QueueState* qs, double now);
    bool ReadRingDwords(QueueState* qs, uint64_t start_dw, uint32_t n,
                        uint32_t* out);
    CopyDir ClassifyDir(QueueState* qs, uint64_t src, uint64_t dst);
    bool IsHostVA(QueueState* qs, uint64_t va);

    Sink sink_;
    SdmaSink sdma_sink_;
    MissSink miss_sink_;
    int poll_us_;
    std::mutex mu_;
    std::vector<std::unique_ptr<QueueState>> queues_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
