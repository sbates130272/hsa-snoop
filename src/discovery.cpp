#include "discovery.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>

namespace hsasnoop {
namespace {

double MonoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool WriteFile(const std::string& path, const std::string& val, bool append) {
    int flags = O_WRONLY | (append ? O_APPEND : O_TRUNC);
    int fd = open(path.c_str(), flags);
    if (fd < 0)
        return false;
    ssize_t n = write(fd, val.data(), val.size());
    close(fd);
    return n == static_cast<ssize_t>(val.size());
}

// Parse "key=value" tokens from an ftrace kprobe line into out.
uint64_t FindU64(const std::string& line, const char* key) {
    std::string needle = std::string(" ") + key + "=";
    size_t p = line.find(needle);
    if (p == std::string::npos)
        return 0;
    p += needle.size();
    return strtoull(line.c_str() + p, nullptr, 10);
}

} // namespace

Discovery::Discovery(std::string tracefs_root, int pid_filter)
    : tracefs_(std::move(tracefs_root)), pid_filter_(pid_filter),
      probe_name_("hsasnoop_cq") {}

Discovery::~Discovery() { Stop(); }

bool Discovery::InstallProbe() {
    const std::string kp = tracefs_ + "/kprobe_events";
    // Remove any stale probe from a previous run (ignore failure).
    WriteFile(kp, "-:" + probe_name_ + "\n", /*append=*/true);

    // arg3 (the kfd_ioctl_create_queue_args*) is in %dx on x86-64 SysV.
    // Offsets come from struct kfd_ioctl_create_queue_args (stable uapi ABI):
    //   ring_base_address    +0   u64
    //   write_pointer_address+8   u64
    //   read_pointer_address +16  u64
    //   ring_size            +32  u32
    //   gpu_id               +36  u32
    //   queue_type           +40  u32
    std::string def = "p:" + probe_name_ +
                      " kfd_ioctl_create_queue"
                      " ring_base=+0(%dx):u64"
                      " wptr=+8(%dx):u64"
                      " rptr=+16(%dx):u64"
                      " ring_size=+32(%dx):u32"
                      " gpu_id=+36(%dx):u32"
                      " qtype=+40(%dx):u32\n";
    if (!WriteFile(kp, def, /*append=*/false)) {
        fprintf(stderr,
                "hsa-snoop: failed to install kprobe (%s). Need root?\n",
                strerror(errno));
        return false;
    }
    if (!WriteFile(tracefs_ + "/events/kprobes/" + probe_name_ + "/enable",
                   "1\n", false)) {
        fprintf(stderr, "hsa-snoop: failed to enable kprobe (%s)\n",
                strerror(errno));
        return false;
    }
    return true;
}

void Discovery::RemoveProbe() {
    WriteFile(tracefs_ + "/events/kprobes/" + probe_name_ + "/enable", "0\n",
              false);
    WriteFile(tracefs_ + "/kprobe_events", "-:" + probe_name_ + "\n",
              /*append=*/true);
}

bool Discovery::Start(Callback cb) {
    cb_ = std::move(cb);
    if (!InstallProbe())
        return false;

    const std::string pipe = tracefs_ + "/trace_pipe";
    pipe_fd_ = open(pipe.c_str(), O_RDONLY | O_NONBLOCK);
    if (pipe_fd_ < 0) {
        fprintf(stderr, "hsa-snoop: cannot open %s: %s\n", pipe.c_str(),
                strerror(errno));
        RemoveProbe();
        return false;
    }
    running_ = true;
    thread_ = std::thread(&Discovery::Run, this);
    return true;
}

void Discovery::Stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    if (pipe_fd_ >= 0) {
        close(pipe_fd_);
        pipe_fd_ = -1;
    }
    RemoveProbe();
}

void Discovery::Run() {
    std::string buf;
    char chunk[8192];
    uint64_t next_uid = 1;

    while (running_) {
        struct pollfd pfd {
            pipe_fd_, POLLIN, 0
        };
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0)
            continue;
        ssize_t n = read(pipe_fd_, chunk, sizeof(chunk));
        if (n <= 0)
            continue;
        buf.append(chunk, n);

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty() || line[0] == '#')
                continue;
            if (line.find(probe_name_) == std::string::npos)
                continue;

            // Parse "<comm>-<pid> [cpu] ..." prefix.
            size_t cpu = line.find(" [");
            if (cpu == std::string::npos)
                continue;
            std::string prefix = line.substr(0, cpu);
            size_t dash = prefix.rfind('-');
            if (dash == std::string::npos)
                continue;
            int pid = atoi(prefix.c_str() + dash + 1);
            std::string comm = prefix.substr(0, dash);
            size_t s = comm.find_first_not_of(" \t");
            if (s != std::string::npos)
                comm = comm.substr(s);

            // The ftrace comm can be a transient value (e.g. "<...>") if the
            // queue is created mid-exec. Prefer the authoritative
            // /proc/<pid>/comm.
            char cpath[64];
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", pid);
            std::ifstream cf(cpath);
            std::string real_comm;
            if (cf && std::getline(cf, real_comm) && !real_comm.empty())
                comm = real_comm;

            if (pid_filter_ && pid != pid_filter_)
                continue;

            QueueInfo q;
            q.pid = pid;
            q.comm = comm;
            q.ring_base = FindU64(line, "ring_base");
            q.wptr_addr = FindU64(line, "wptr");
            q.rptr_addr = FindU64(line, "rptr");
            q.ring_size = static_cast<uint32_t>(FindU64(line, "ring_size"));
            q.gpu_id = static_cast<uint32_t>(FindU64(line, "gpu_id"));
            q.qtype = static_cast<uint32_t>(FindU64(line, "qtype"));
            q.create_ts = MonoNow();
            q.uid = next_uid++;

            if (q.ring_base && q.ring_size && cb_)
                cb_(q);
        }
    }
}

} // namespace hsasnoop
