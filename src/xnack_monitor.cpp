// xnack_monitor.cpp - AMD GPU XNACK page-fault event monitor via bpftrace.
//
// Forks bpftrace with a script that attaches a kprobe to kfd_process_vm_fault
// in the amdgpu/KFD driver. For each invocation the script prints a structured
// line that is parsed here and delivered to the caller via a sink.
//
// Output line format:
//   XNACK pid=<n> comm=<s> pasid=<n>
//
// kfd_process_vm_fault signature:
//   int kfd_process_vm_fault(struct device_queue_manager *dqm, u32 pasid)
//   arg0 = dqm   (struct device_queue_manager *)
//   arg1 = pasid (u32) — AMD Process ASID identifying the GPU context
#include "xnack_monitor.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hsasnoop {

// ---------------------------------------------------------------------------
// bpftrace script
// ---------------------------------------------------------------------------

std::string XnackMonitor::BuildBpftraceScript() {
    return R"(
#!/usr/bin/env bpftrace

// kfd_process_vm_fault(struct device_queue_manager *dqm, u32 pasid)
// arg0 = dqm    (struct device_queue_manager *)
// arg1 = pasid  (u32) — AMD Process ASID for the faulting GPU context
//
// This probe fires for every GPU page fault delivered to the KFD driver
// through the XNACK (retry) path on gfx900+ (Vega10+) hardware.

BEGIN {
    printf("XNACK_MONITOR_READY\n");
}

kprobe:kfd_process_vm_fault {
    $pasid = (uint32)arg1;
    printf("XNACK pid=%d comm=%s pasid=%u\n", pid, comm, $pasid);
}
)";
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool XnackMonitor::Start(Sink sink) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: run bpftrace. Redirect stdout to pipe.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        // Redirect stderr to /dev/null to suppress bpftrace noise.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Write the bpftrace script to a temp file and pass it as an argument.
        // bpftrace does not support stdin scripts reliably across versions,
        // so we write to /tmp and exec with the path.
        char tmppath[] = "/tmp/xnack-snoop-XXXXXX.bt";
        int tmpfd = mkstemps(tmppath, 3);
        if (tmpfd < 0)
            _exit(1);
        std::string script = BuildBpftraceScript();
        if (write(tmpfd, script.data(), script.size()) < 0) {
            close(tmpfd);
            _exit(1);
        }
        close(tmpfd);
        execlp("bpftrace", "bpftrace", tmppath, nullptr);
        // bpftrace not found or exec failed — clean up and exit.
        unlink(tmppath);
        _exit(127);
    }

    // Parent: close write end, keep read end.
    close(pipefd[1]);
    bpftrace_pid_ = pid;
    bpftrace_stdout_ = pipefd[0];

    running_ = true;
    reader_thread_ = std::thread(
        [this, s = std::move(sink)]() mutable { ReadLoop(std::move(s)); });
    return true;
}

void XnackMonitor::Stop() {
    if (!running_.exchange(false))
        return;
    if (bpftrace_pid_ > 0) {
        kill(bpftrace_pid_, SIGTERM);
        // Give bpftrace a moment to flush END block output.
        usleep(200 * 1000);
        int status;
        waitpid(bpftrace_pid_, &status, WNOHANG);
        kill(bpftrace_pid_, SIGKILL);
        waitpid(bpftrace_pid_, nullptr, 0);
        bpftrace_pid_ = -1;
    }
    if (bpftrace_stdout_ >= 0) {
        close(bpftrace_stdout_);
        bpftrace_stdout_ = -1;
    }
    if (reader_thread_.joinable())
        reader_thread_.join();
}

// ---------------------------------------------------------------------------
// ReadLoop — parse bpftrace output lines into XnackRecord
// ---------------------------------------------------------------------------

void XnackMonitor::ReadLoop(Sink sink) {
    FILE* fp = fdopen(bpftrace_stdout_, "r");
    if (!fp) {
        running_ = false;
        return;
    }

    uint64_t seq = 0;
    char line[512];
    bool ready = false;

    while (fgets(line, sizeof(line), fp)) {
        if (!running_)
            break;

        // Wait for the ready sentinel before processing events.
        if (!ready) {
            if (strstr(line, "XNACK_MONITOR_READY"))
                ready = true;
            continue;
        }

        // Parse: XNACK pid=<n> comm=<s> pasid=<n>
        if (strncmp(line, "XNACK ", 6) != 0)
            continue;

        XnackRecord rec;
        rec.seq = ++seq;

        char comm[256] = {};
        unsigned pasid = 0;

        int n = sscanf(line, "XNACK pid=%d comm=%255s pasid=%u", &rec.pid,
                       comm, &pasid);
        if (n < 3)
            continue;

        rec.comm = comm;
        rec.pasid = pasid;

        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        rec.timestamp = ts.tv_sec + ts.tv_nsec * 1e-9;

        sink(rec);
    }

    fclose(fp);
    bpftrace_stdout_ = -1;
    running_ = false;
}

} // namespace hsasnoop
