// ais_monitor.h - AMD Infinity Storage (AIS) IO monitor.
//
// Forks bpftrace with kprobe/kretprobe scripts attached to kfd_ioctl_ais (the
// KFD ioctl handler for AMDKFD_IOC_AIS_OP) to observe AIS read/write
// operations across all processes. For each completed ioctl, an AisRecord is
// emitted to the caller via a sink callback.
//
// Probed functions (all in amdgpu.ko):
//   kprobe:kfd_ioctl_ais    — captures op, size, gpu_id, pid, comm, timestamp
//   kretprobe:kfd_ioctl_ais — computes latency, captures size_copied + status
//                             from the output region of the kernel args buffer
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "model.h"

namespace hsasnoop {

class AisMonitor {
  public:
    using Sink = std::function<void(const AisRecord&)>;

    // Starts bpftrace in the background. sink is called from a background
    // thread for each completed AIS ioctl. Returns false if bpftrace cannot
    // be started.
    bool Start(Sink sink);

    // Stops bpftrace and joins the reader thread. Safe to call multiple times.
    void Stop();

    ~AisMonitor() { Stop(); }

  private:
    void ReadLoop(Sink sink);
    static std::string BuildBpftraceScript();

    pid_t bpftrace_pid_ = -1;
    int bpftrace_stdout_ = -1; // read end of pipe from bpftrace stdout
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
