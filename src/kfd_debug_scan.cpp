#include "kfd_debug_scan.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/kfd_ioctl.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace hsasnoop {

static std::atomic<uint64_t> s_next_uid{1};

uint64_t AllocQueueUid() {
    return s_next_uid.fetch_add(1, std::memory_order_relaxed);
}

namespace {

double MonoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Return all PIDs that have /dev/kfd open, optionally filtered to pid_filter.
std::vector<pid_t> FindKfdProcesses(pid_t pid_filter) {
    std::vector<pid_t> result;
    DIR* proc = opendir("/proc");
    if (!proc)
        return result;

    struct dirent* ent;
    while ((ent = readdir(proc)) != nullptr) {
        if (ent->d_type != DT_DIR && ent->d_type != DT_UNKNOWN)
            continue;
        pid_t pid = static_cast<pid_t>(atoi(ent->d_name));
        if (pid <= 0)
            continue;
        if (pid_filter && pid != pid_filter)
            continue;

        char fddir[64];
        snprintf(fddir, sizeof(fddir), "/proc/%d/fd", pid);
        DIR* fds = opendir(fddir);
        if (!fds)
            continue;

        bool found = false;
        struct dirent* fe;
        while (!found && (fe = readdir(fds)) != nullptr) {
            char fdpath[300], target[256];
            snprintf(fdpath, sizeof(fdpath), "/proc/%d/fd/%s", pid, fe->d_name);
            ssize_t n = readlink(fdpath, target, sizeof(target) - 1);
            if (n > 0) {
                target[n] = '\0';
                if (strcmp(target, "/dev/kfd") == 0)
                    found = true;
            }
        }
        closedir(fds);
        if (found)
            result.push_back(pid);
    }
    closedir(proc);
    return result;
}

// Snapshot all KFD queues for pid using the debug-trap API.
// kfd_fd must be an open fd on /dev/kfd in this process.
void SnapshotProcessQueues(pid_t pid, int kfd_fd,
                           const std::function<void(const QueueInfo&)>& cb) {
    int efd = eventfd(0, EFD_CLOEXEC);
    if (efd < 0) {
        fprintf(stderr, "[kfd-scan] eventfd failed for pid=%d: %s\n", pid,
                strerror(errno));
        return;
    }

    if (ptrace(PTRACE_SEIZE, pid, 0, 0) < 0) {
        if (errno != EPERM && errno != ESRCH)
            fprintf(stderr, "[kfd-scan] ptrace SEIZE pid=%d: %s\n", pid,
                    strerror(errno));
        close(efd);
        return;
    }

    // Enable debug mode on target.
    struct kfd_ioctl_dbg_trap_args args{};
    args.pid = static_cast<__u32>(pid);
    args.op = KFD_IOC_DBG_TRAP_ENABLE;
    args.enable.exception_mask = 0;
    args.enable.rinfo_ptr = 0;
    args.enable.rinfo_size = 0;
    args.enable.dbg_fd = static_cast<__u32>(efd);

    if (ioctl(kfd_fd, AMDKFD_IOC_DBG_TRAP, &args) < 0) {
        if (errno != ESRCH && errno != EINVAL)
            fprintf(stderr, "[kfd-scan] DBG_TRAP_ENABLE pid=%d: %s\n", pid,
                    strerror(errno));
        ptrace(PTRACE_DETACH, pid, 0, 0);
        close(efd);
        return;
    }

    // Two-pass GET_QUEUE_SNAPSHOT: first to count, then to fill.
    struct kfd_ioctl_dbg_trap_args snap{};
    snap.pid = static_cast<__u32>(pid);
    snap.op = KFD_IOC_DBG_TRAP_GET_QUEUE_SNAPSHOT;
    snap.queue_snapshot.exception_mask = 0;
    snap.queue_snapshot.snapshot_buf_ptr = 0;
    snap.queue_snapshot.num_queues = 0;
    snap.queue_snapshot.entry_size =
        static_cast<__u32>(sizeof(struct kfd_queue_snapshot_entry));

    if (ioctl(kfd_fd, AMDKFD_IOC_DBG_TRAP, &snap) < 0) {
        fprintf(stderr, "[kfd-scan] GET_QUEUE_SNAPSHOT (count) pid=%d: %s\n",
                pid, strerror(errno));
        goto disable;
    }

    if (snap.queue_snapshot.num_queues > 0) {
        uint32_t nq = snap.queue_snapshot.num_queues;
        std::vector<struct kfd_queue_snapshot_entry> entries(nq);

        snap.queue_snapshot.snapshot_buf_ptr =
            reinterpret_cast<__u64>(entries.data());
        snap.queue_snapshot.num_queues = nq;
        snap.queue_snapshot.entry_size =
            static_cast<__u32>(sizeof(struct kfd_queue_snapshot_entry));

        if (ioctl(kfd_fd, AMDKFD_IOC_DBG_TRAP, &snap) < 0) {
            fprintf(stderr, "[kfd-scan] GET_QUEUE_SNAPSHOT (fill) pid=%d: %s\n",
                    pid, strerror(errno));
            goto disable;
        }

        // Read process name once.
        std::string comm;
        {
            char cpath[64];
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", pid);
            std::ifstream cf(cpath);
            std::getline(cf, comm);
        }

        double ts = MonoNow();
        uint32_t filled = snap.queue_snapshot.num_queues;
        for (uint32_t i = 0; i < filled && i < nq; ++i) {
            const auto& e = entries[i];
            if (!e.ring_base_address || !e.ring_size)
                continue;
            QueueInfo q{};
            q.pid = pid;
            q.comm = comm;
            q.ring_base = e.ring_base_address;
            q.wptr_addr = e.write_pointer_address;
            q.rptr_addr = e.read_pointer_address;
            q.ring_size = e.ring_size;
            q.gpu_id = e.gpu_id;
            q.qtype = e.queue_type;
            q.create_ts = ts;
            q.uid = AllocQueueUid();
            cb(q);
        }
    }

disable:
    struct kfd_ioctl_dbg_trap_args dis{};
    dis.pid = static_cast<__u32>(pid);
    dis.op = KFD_IOC_DBG_TRAP_DISABLE;
    ioctl(kfd_fd, AMDKFD_IOC_DBG_TRAP, &dis); // best-effort; ignore error

    ptrace(PTRACE_DETACH, pid, 0, 0);
    close(efd);
}

} // namespace

int ScanExistingKfdQueues(std::function<void(const QueueInfo&)> cb,
                          pid_t pid_filter) {
    int kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
    if (kfd_fd < 0) {
        fprintf(stderr, "[kfd-scan] cannot open /dev/kfd: %s\n",
                strerror(errno));
        return 0;
    }

    std::vector<pid_t> pids = FindKfdProcesses(pid_filter);
    int scanned = 0;
    for (pid_t pid : pids) {
        SnapshotProcessQueues(pid, kfd_fd, cb);
        ++scanned;
    }

    close(kfd_fd);
    return scanned;
}

} // namespace hsasnoop
