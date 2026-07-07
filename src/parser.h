// parser.h - Per-queue AQL ring poller. Watches write/read dispatch ids in the
// target process and decodes packets as they are enqueued and completed.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "ksym.h"
#include "model.h"

namespace hsasnoop {

class RingParser {
 public:
  using Sink = std::function<void(const PacketRecord&)>;

  // poll_us: sleep between polls (microseconds). Lower catches faster kernels.
  RingParser(Sink sink, int poll_us = 20);
  ~RingParser();

  // Begin polling a newly discovered queue (thread-safe).
  void AddQueue(const QueueInfo& q);
  void Stop();

 private:
  struct QueueState {
    QueueInfo info;
    std::unique_ptr<KernelSymbolResolver> ksym;
    uint64_t next_id = 0;   // next dispatch id to decode
    uint64_t last_rptr = 0;
    bool primed = false;
    uint64_t dropped = 0;
    std::map<uint64_t, PacketRecord> inflight;  // id -> record
  };

  void Run();
  void PollQueue(QueueState* qs, double now);
  bool DecodeSlot(QueueState* qs, uint64_t id, PacketRecord* rec);

  Sink sink_;
  int poll_us_;
  std::mutex mu_;
  std::vector<std::unique_ptr<QueueState>> queues_;
  std::thread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace hsasnoop
