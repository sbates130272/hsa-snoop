// ais_monitor.h - AMD Infinity Storage (AIS) IO monitor.
//
// Installs tracefs probes on kfd_ioctl_ais (the KFD ioctl handler for
// AMDKFD_IOC_AIS_OP) and reads their output from trace_pipe: an entry probe
// for the request and a return probe for its status, matched by TID, so each
// AisRecord says whether the transfer succeeded. If the return probe cannot be
// installed, records are emitted at entry and assumed to succeed. Uses the
// same tracefs mechanism as discovery.cpp rather than bpftrace, which silently
// fails to hook DKMS module symbols on some kernel+bpftrace version
// combinations.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <unordered_map>

#include "model.h"

namespace hsasnoop {

// One line of the AIS probes' trace_pipe output.
struct AisTraceLine {
    enum class Kind { Entry, Return } kind = Kind::Entry;
    int tid = 0;
    std::string comm;
    double ts = 0; // trace clock, seconds
    // Entry only.
    uint32_t gpu_id = 0;
    uint64_t size_req = 0;
    uint32_t op = 0;
    int fd = -1;
    // Return only: the ioctl's return value (0 or a negative errno).
    int ret = 0;
};

// Parses a trace_pipe line produced by the probe named entry_probe or
// return_probe (return_probe may be empty). Returns false for any other line.
bool ParseAisTraceLine(const std::string& line, const std::string& entry_probe,
                       const std::string& return_probe, AisTraceLine* out);

class AisMonitor {
  public:
    using Sink = std::function<void(const AisRecord&)>;

    explicit AisMonitor(std::string tracefs = "/sys/kernel/tracing")
        : tracefs_(std::move(tracefs)) {}

    // Installs kprobes and starts the reader thread.  sink is called from a
    // background thread for each completed AIS ioctl.  Returns false if the
    // kprobes cannot be installed (not root, or kfd_ioctl_ais absent).
    bool Start(Sink sink);

    // Removes kprobes and joins the reader thread.  Safe to call multiple
    // times.
    void Stop();

    ~AisMonitor() { Stop(); }

  private:
    bool InstallKprobes();
    void RemoveKprobes();
    void ReadLoop(Sink sink);
    void HandleLine(const std::string& line, const Sink& sink);
    static void EmitAssumedOk(AisRecord rec, const Sink& sink);

    std::string tracefs_;
    std::string instance_;     // dedicated tracefs instance path
    std::string probe_entry_;  // tracefs event name for entry probe
    std::string probe_return_; // return probe; empty if not installed
    // Requests that have entered the ioctl and not yet returned, by TID.
    struct Pending {
        AisRecord rec;
        double entry_trace_ts = 0;
    };
    std::unordered_map<int, Pending> pending_;
    uint64_t seq_ = 0;
    int trace_pipe_fd_ = -1;
    int cancel_wfd_ = -1; // write end of cancellation pipe
    int cancel_rfd_ = -1; // read end of cancellation pipe
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
