// ais_monitor.h - AMD Infinity Storage (AIS) IO monitor.
//
// Installs two tracefs kprobes (entry + return) on kfd_ioctl_ais (the KFD
// ioctl handler for AMDKFD_IOC_AIS_OP) and reads their output from
// trace_pipe.  Correlates entry and return by TID to emit a complete
// AisRecord per operation.  Uses the same tracefs mechanism as discovery.cpp
// rather than bpftrace, which silently fails to hook DKMS module symbols on
// some kernel+bpftrace version combinations.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include "model.h"

namespace hsasnoop {

class AisMonitor {
  public:
    using Sink = std::function<void(const AisRecord&)>;

    explicit AisMonitor(std::string tracefs = "/sys/kernel/tracing")
        : tracefs_(std::move(tracefs)) {}

    // Installs kprobes and starts the reader thread.  sink is called from a
    // background thread for each completed AIS ioctl.  Returns false if the
    // kprobes cannot be installed (not root, or kfd_ioctl_ais absent).
    bool Start(Sink sink);

    // Removes kprobes and joins the reader thread.  Safe to call multiple times.
    void Stop();

    ~AisMonitor() { Stop(); }

  private:
    bool InstallKprobes();
    void RemoveKprobes();
    void ReadLoop(Sink sink);

    std::string tracefs_;
    std::string instance_;    // dedicated tracefs instance path
    std::string probe_entry_; // tracefs event name for entry probe
    int trace_pipe_fd_ = -1;
    int cancel_wfd_ = -1;     // write end of cancellation pipe
    int cancel_rfd_ = -1;     // read end of cancellation pipe
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
