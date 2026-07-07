// discovery.h - Queue discovery via a dynamic ftrace kprobe on the amdgpu KFD
// create-queue ioctl. No driver modification and no bpftrace/libbpf runtime
// dependency: we install a kprobe by writing to tracefs, then stream events
// from trace_pipe.
#pragma once
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include "model.h"

namespace hsasnoop {

class Discovery {
 public:
  using Callback = std::function<void(const QueueInfo&)>;

  // tracefs_root is typically /sys/kernel/tracing. pid_filter != 0 restricts
  // discovery to a single process.
  explicit Discovery(std::string tracefs_root = "/sys/kernel/tracing",
                     int pid_filter = 0);
  ~Discovery();

  // Install the kprobe and start streaming. cb is invoked for each AQL queue.
  bool Start(Callback cb);
  void Stop();

 private:
  void Run();
  bool InstallProbe();
  void RemoveProbe();

  std::string tracefs_;
  int pid_filter_;
  std::string probe_name_;
  Callback cb_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  int pipe_fd_ = -1;
};

}  // namespace hsasnoop
