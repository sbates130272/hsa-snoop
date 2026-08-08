// xnack_monitor.h - AMD GPU XNACK (page-fault retry) event monitor.
//
// Forks bpftrace with a kprobe attached to kfd_process_vm_fault (in the
// amdgpu KFD driver) to count GPU page-fault events that go through the XNACK
// retry path. For each fault, an XnackRecord is emitted to the caller via a
// sink callback.
//
// Probed function (in amdgpu.ko):
//   kprobe:kfd_process_vm_fault — fires on every GPU VM fault delivered to KFD
//
// XNACK ("eXtended Non-ACKnowledgement") is the AMD GPU mechanism by which
// Vega10+ GPUs (gfx900+) retry a faulting memory access rather than aborting.
// When the GPU cannot translate a virtual address, it signals a fault; the KFD
// driver resolves or migrates the page and lets the GPU retry. Every call to
// kfd_process_vm_fault represents one such retry-eligible fault event.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "model.h"

namespace hsasnoop {

class XnackMonitor {
  public:
    using Sink = std::function<void(const XnackRecord&)>;

    // Starts bpftrace in the background. sink is called from a background
    // thread for each GPU page-fault event. Returns false if bpftrace cannot
    // be started.
    bool Start(Sink sink);

    // Stops bpftrace and joins the reader thread. Safe to call multiple times.
    void Stop();

    ~XnackMonitor() { Stop(); }

  private:
    void ReadLoop(Sink sink);
    static std::string BuildBpftraceScript();

    pid_t bpftrace_pid_ = -1;
    int bpftrace_stdout_ = -1; // read end of pipe from bpftrace stdout
    std::thread reader_thread_;
    std::atomic<bool> running_{false};
};

} // namespace hsasnoop
