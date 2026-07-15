// discovery.h - Queue discovery via either:
//   kprobe mode  : ftrace kprobe on kfd_ioctl_create_queue (native amdgpu/KFD)
//   bpftrace mode: uprobe on hsaKmtCreateQueueExt in librocdxg.so (WSL2/WDDM)
#pragma once
#include "model.h"
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace hsasnoop {

enum class DiscoveryMode {
    kKprobe,   // default: kfd_ioctl_create_queue kprobe via tracefs
    kBpftrace, // WSL2/librocdxg: uprobe on hsaKmtCreateQueueExt via bpftrace
};

// Returns true when running inside a WSL2 environment.
bool IsWsl2();

// Finds the real (non-symlink) path of librocdxg.so installed on the system,
// or returns empty string if not found.
std::string FindLibrocdxg();

class Discovery {
  public:
    using Callback = std::function<void(const QueueInfo&)>;

    // tracefs_root is typically /sys/kernel/tracing. pid_filter != 0 restricts
    // discovery to a single process.
    explicit Discovery(std::string tracefs_root = "/sys/kernel/tracing",
                       int pid_filter = 0,
                       DiscoveryMode mode = DiscoveryMode::kKprobe,
                       std::string librocdxg_path = "");
    ~Discovery();

    // Install the probe and start streaming. cb is invoked for each AQL queue.
    bool Start(Callback cb);
    void Stop();

  private:
    // kprobe path
    void RunKprobe();
    bool InstallKprobe();
    void RemoveKprobe();

    // bpftrace path
    void RunBpftrace();
    bool StartBpftrace();

    std::string tracefs_;
    int pid_filter_;
    DiscoveryMode mode_;
    std::string librocdxg_path_;
    std::string probe_name_;
    Callback cb_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    int pipe_fd_ = -1;        // kprobe: trace_pipe fd; bpftrace: output pipe fd
    pid_t bpf_pid_ = -1;      // bpftrace child PID (bpftrace mode only)
    std::string bpf_prebuf_;  // data read during arm-detection
    std::string bpf_outpath_; // path to bpftrace temp script file
    // True if we flipped the global tracefs `tracing_on` from 0->1 (some hosts
    // ship it disabled, which silently suppresses all kprobe events); restored
    // to 0 on RemoveKprobe so we leave the system as we found it.
    bool tracing_on_changed_ = false;
};

} // namespace hsasnoop
