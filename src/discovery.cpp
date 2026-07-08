#include "discovery.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
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

uint64_t FindU64(const std::string& line, const char* key) {
    std::string needle = std::string(" ") + key + "=";
    size_t p = line.find(needle);
    if (p == std::string::npos)
        return 0;
    p += needle.size();
    return strtoull(line.c_str() + p, nullptr, 10);
}

// Return real path of a file, resolving symlinks. Returns path unchanged if
// realpath() fails (e.g. file does not exist).
std::string Realpath(const std::string& path) {
    char buf[4096];
    if (::realpath(path.c_str(), buf))
        return buf;
    return path;
}

// Check whether path exists and is a regular file or shared library.
bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace

// ---------------------------------------------------------------------------
// Public helpers

bool IsWsl2() {
    std::ifstream f("/proc/version");
    std::string line;
    if (!std::getline(f, line))
        return false;
    return line.find("microsoft") != std::string::npos ||
           line.find("Microsoft") != std::string::npos;
}

std::string FindLibrocdxg() {
    // Resolve the canonical path the HSA runtime actually dlopen()s.
    // The runtime uses "librocdxg.so" (no version suffix) via LD_LIBRARY_PATH
    // or /opt/rocm/lib, which resolves through two symlink hops:
    //   /opt/rocm/lib/librocdxg.so -> librocdxg.so.1 -> librocdxg.so.1.2.x
    // We must probe the real .so.* file (not a symlink) because the kernel
    // uprobe infrastructure resolves paths to inodes, and a symlink target
    // that differs from the probed path will cause the probe to miss.
    static const char* kCandidates[] = {
        "/opt/rocm/lib/librocdxg.so", // follows symlinks to the real file
        "/opt/rocm/lib/librocdxg.so.1",
        "/usr/local/src/librocdxg/build/librocdxg.so.1.2.0",
        nullptr,
    };
    for (const char** p = kCandidates; *p; ++p) {
        std::string real = Realpath(*p);
        if (FileExists(real))
            return real;
    }
    // Fall back: search /opt -name 'librocdxg.so.*' for a real file.
    // -print -quit stops after the first match, avoiding a full traversal.
    FILE* fp = popen("find /opt -maxdepth 4 -name 'librocdxg.so.*' -not -type l"
                     " -print -quit 2>/dev/null",
                     "r");
    if (fp) {
        char buf[512] = {};
        if (fgets(buf, sizeof(buf) - 1, fp)) {
            buf[strcspn(buf, "\n")] = '\0';
            pclose(fp);
            if (buf[0])
                return buf;
        } else {
            pclose(fp);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Discovery

Discovery::Discovery(std::string tracefs_root, int pid_filter,
                     DiscoveryMode mode, std::string librocdxg_path)
    : tracefs_(std::move(tracefs_root)), pid_filter_(pid_filter), mode_(mode),
      librocdxg_path_(std::move(librocdxg_path)),
      probe_name_("hsasnoop_cq_" + std::to_string(getpid())) {}

Discovery::~Discovery() { Stop(); }

bool Discovery::Start(Callback cb) {
    cb_ = std::move(cb);
    if (mode_ == DiscoveryMode::kBpftrace) {
        if (!StartBpftrace())
            return false;
        running_ = true;
        thread_ = std::thread(&Discovery::RunBpftrace, this);
        return true;
    }
    if (!InstallKprobe())
        return false;
    const std::string pipe = tracefs_ + "/trace_pipe";
    pipe_fd_ = open(pipe.c_str(), O_RDONLY | O_NONBLOCK);
    if (pipe_fd_ < 0) {
        fprintf(stderr, "hsa-snoop: cannot open %s: %s\n", pipe.c_str(),
                strerror(errno));
        RemoveKprobe();
        return false;
    }
    running_ = true;
    thread_ = std::thread(&Discovery::RunKprobe, this);
    return true;
}

void Discovery::Stop() {
    if (mode_ == DiscoveryMode::kBpftrace) {
        if (!running_.exchange(false))
            return;
        // Kill bpftrace. It will exit and close the pipe write-end, causing
        // RunBpftrace's blocking read() to return 0 (EOF), and the thread
        // exits.
        if (bpf_pid_ > 0) {
            kill(bpf_pid_, SIGTERM);
            int status;
            waitpid(bpf_pid_, &status, 0);
            bpf_pid_ = -1;
        }
        if (thread_.joinable())
            thread_.join();
        if (!bpf_outpath_.empty()) {
            unlink(bpf_outpath_.c_str());
            bpf_outpath_.clear();
        }
    } else {
        if (!running_.exchange(false))
            return;
        if (thread_.joinable())
            thread_.join();
        RemoveKprobe();
    }
    if (pipe_fd_ >= 0) {
        close(pipe_fd_);
        pipe_fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// kprobe mode

bool Discovery::InstallKprobe() {
    const std::string kp = tracefs_ + "/kprobe_events";
    WriteFile(kp, "-:" + probe_name_ + "\n", /*append=*/true);

    // arg3 (the kfd_ioctl_create_queue_args*) is in %dx on x86-64 SysV.
    // Offsets from struct kfd_ioctl_create_queue_args (stable uapi ABI):
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
    if (!WriteFile(kp, def, /*append=*/true)) {
        fprintf(stderr,
                "hsa-snoop: failed to install kprobe (%s). Need root?\n",
                strerror(errno));
        return false;
    }
    if (!WriteFile(tracefs_ + "/events/kprobes/" + probe_name_ + "/enable",
                   "1\n", false)) {
        fprintf(stderr, "hsa-snoop: failed to enable kprobe (%s)\n",
                strerror(errno));
        RemoveKprobe();
        return false;
    }
    return true;
}

void Discovery::RemoveKprobe() {
    WriteFile(tracefs_ + "/events/kprobes/" + probe_name_ + "/enable", "0\n",
              false);
    WriteFile(tracefs_ + "/kprobe_events", "-:" + probe_name_ + "\n",
              /*append=*/true);
}

void Discovery::RunKprobe() {
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

// ---------------------------------------------------------------------------
// bpftrace mode (WSL2 / librocdxg)
//
// We fork bpftrace with a script that uprobes hsaKmtCreateQueueExt in
// librocdxg.so.  At entry, the HsaQueueResource* (stack arg 9, i.e. the 3rd
// argument beyond the 6 register args) already has Queue_write_ptr_aql and
// Queue_read_ptr_aql populated by the caller (the ROCr AQL queue ctor fills
// them before calling into the thunk).
//
// bpftrace output format (one line per AQL queue):
//   QUEUE pid=<N> comm=<s> ring=<hex> wptr=<hex> rptr=<hex> size=<dec> node=<N>
//
// Argument layout for hsaKmtCreateQueueExt (x86-64 SysV ABI):
//   arg0 = NodeId (rdi, u32)
//   arg1 = Type   (rsi, HSA_QUEUE_TYPE)
//   arg2 = QueuePercentage (rdx)
//   arg3 = Priority (rcx)
//   arg4 = SdmaEngineId (r8)
//   arg5 = QueueAddress (r9) = ring buffer VA
// Stack args (past rsp at function entry; bpftrace sptr points to saved rip):
//   *(sptr+8)  = QueueSizeInBytes (u64)
//   *(sptr+16) = Event* (HsaEvent*)
//   *(sptr+24) = QueueResource* (HsaQueueResource*)  <- OUT
// HsaQueueResource layout:
//   +0  QueueId (u64)
//   +8  doorbell ptr (u64)
//   +16 Queue_write_ptr_aql (u64* -> wptr_addr)
//   +24 Queue_read_ptr_aql  (u64* -> rptr_addr)
//   +32 ErrorReason (ptr)
//
// HSA_QUEUE_COMPUTE_AQL == 21 (from hsakmttypes.h).

static std::string BuildBpftraceScript(const std::string& so_path,
                                       int pid_filter) {
    // Note: pid_filter is a WSL2 namespace PID, but bpftrace sees host PIDs
    // (which differ by a fixed offset ~93000 on Strix Halo WSL2). We cannot
    // filter by PID in the bpftrace script; filtering is done in RunBpftrace().
    (void)pid_filter;
    std::ostringstream s;
    s << "uprobe:" << so_path << ":hsaKmtCreateQueueExt\n";
    s << "{\n";
    // Only trace AQL compute queues (Type == HSA_QUEUE_COMPUTE_AQL == 21).
    s << "  if ((int32)arg1 != 21) { return; }\n";
    // Read HsaQueueResource* from the third stack argument.
    // reg("sp") gives the stack pointer at the uprobe site (just after CALL
    // pushed the return address). Stack layout: [sp+0]=retaddr,
    // [sp+8]=QueueSizeInBytes, [sp+16]=Event*, [sp+24]=QueueResource*.
    // Note: bpftrace 0.20.x uses reg("sp"); newer versions support sptr.
    s << "  $sp = reg(\"sp\");\n";
    s << "  $rsrc = *(uint64*)($sp + 24);\n";
    s << "  $wptr = *(uint64*)($rsrc + 16);\n";
    s << "  $rptr = *(uint64*)($rsrc + 24);\n";
    s << "  $size = *(uint64*)($sp + 8);\n";
    // arg5 is QueueAddress (r9) = ring buffer VA. arg0 is NodeId.
    s << "  printf(\"QUEUE pid=%d comm=%s ring=0x%lx wptr=0x%lx rptr=0x%lx"
         " size=%lu node=%u\\n\",\n";
    s << "         pid, comm, (uint64)arg5, $wptr, $rptr, $size, "
         "(uint32)arg0);\n";
    s << "}\n";
    // An interval probe fires periodically and triggers a buffer flush,
    // providing real-time printf output delivery. Use 1ms for minimal latency.
    s << "interval:ms:1 { }\n";
    return s.str();
}

bool Discovery::StartBpftrace() {
    if (librocdxg_path_.empty()) {
        fprintf(stderr, "hsa-snoop: bpftrace mode requires librocdxg.so path "
                        "(set --librocdxg or use auto-detect)\n");
        return false;
    }
    if (!FileExists(librocdxg_path_)) {
        fprintf(stderr, "hsa-snoop: librocdxg not found at %s\n",
                librocdxg_path_.c_str());
        return false;
    }

    // Write the bpftrace script to a temp file.
    char tmppath[] = "/tmp/hsa-snoop-XXXXXX.bt";
    int tmpfd = mkstemps(tmppath, 3);
    if (tmpfd < 0) {
        fprintf(stderr, "hsa-snoop: cannot create temp script: %s\n",
                strerror(errno));
        return false;
    }
    std::string script = BuildBpftraceScript(librocdxg_path_, pid_filter_);
    if (write(tmpfd, script.data(), script.size()) !=
        static_cast<ssize_t>(script.size())) {
        fprintf(stderr, "hsa-snoop: cannot write bpftrace script\n");
        close(tmpfd);
        unlink(tmppath);
        return false;
    }
    close(tmpfd);

    // bpftrace writes printf output to stdout. With an `interval:ms:1` probe
    // in the script, bpftrace flushes its output buffer frequently, giving
    // real-time output even when stdout is a pipe.
    int fds[2];
    if (pipe(fds) < 0) {
        fprintf(stderr, "hsa-snoop: pipe: %s\n", strerror(errno));
        unlink(tmppath);
        return false;
    }

    bpf_pid_ = fork();
    if (bpf_pid_ < 0) {
        fprintf(stderr, "hsa-snoop: fork: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        unlink(tmppath);
        return false;
    }

    if (bpf_pid_ == 0) {
        // stdout → pipe (data channel). Suppress stderr unless debug.
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        if (!getenv("HSA_SNOOP_DEBUG")) {
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                dup2(dn, STDERR_FILENO);
                close(dn);
            }
        }
        execlp("bpftrace", "bpftrace", "-B", "none", tmppath, nullptr);
        perror("bpftrace");
        _exit(127);
    }

    close(fds[1]);
    pipe_fd_ = fds[0];
    // NOTE: do NOT unlink(tmppath) yet — bpftrace opens it by path.

    // Wait for "Attaching..." on stdout to confirm the uprobe is armed.
    {
        fcntl(pipe_fd_, F_SETFL, O_NONBLOCK);
        char chunk[512];
        bool armed = false;
        double deadline = MonoNow() + 20.0;
        while (MonoNow() < deadline) {
            struct pollfd pfd {
                pipe_fd_, POLLIN, 0
            };
            poll(&pfd, 1, 200);
            ssize_t n = read(pipe_fd_, chunk, sizeof(chunk));
            if (n > 0) {
                bpf_prebuf_.append(chunk, n);
                if (bpf_prebuf_.find("Attaching") != std::string::npos) {
                    armed = true;
                    unlink(tmppath);
                    break;
                }
            }
            // Check if bpftrace exited (script error etc.).
            int status;
            pid_t r = waitpid(bpf_pid_, &status, WNOHANG);
            if (r == bpf_pid_) {
                fprintf(stderr,
                        "hsa-snoop: bpftrace exited during startup (exit=%d)\n"
                        "hsa-snoop: ensure bpftrace is installed and you are "
                        "root.\n",
                        WEXITSTATUS(status));
                close(pipe_fd_);
                pipe_fd_ = -1;
                bpf_pid_ = -1;
                unlink(tmppath);
                return false;
            }
        }
        if (!armed) {
            unlink(tmppath);
            fprintf(stderr, "hsa-snoop: bpftrace did not attach in time; "
                            "some early queues may be missed\n");
        }
        // Switch to blocking so RunBpftrace uses simple blocking read().
        fcntl(pipe_fd_, F_SETFL, 0);
    }

    fprintf(stderr,
            "hsa-snoop: bpftrace armed (pid=%d), watching for AQL queues "
            "in %s...\n",
            bpf_pid_, librocdxg_path_.c_str());
    return true;
}

void Discovery::RunBpftrace() {
    // Start with any data drained during arm-detection (may include QUEUE
    // lines).
    std::string buf = std::move(bpf_prebuf_);
    char chunk[8192];
    uint64_t next_uid = 1;
    bool eof = false;
    const bool dbg = getenv("HSA_SNOOP_DEBUG") != nullptr;
    if (dbg)
        fprintf(stderr, "[ksym bpftrace] start fd=%d prebuf=%zu bytes\n",
                pipe_fd_, buf.size());

    while (!eof) {
        if (buf.empty()) {
            ssize_t n = read(pipe_fd_, chunk, sizeof(chunk));
            if (n > 0) {
                buf.append(chunk, n);
            } else if (n == 0) {
                eof = true;
                break;
            } else {
                if (errno == EINTR)
                    continue;
                break;
            }
        }

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty())
                continue;

            if (getenv("HSA_SNOOP_DEBUG"))
                fprintf(stderr, "[bpftrace] %s\n", line.c_str());

            // Expected: "QUEUE pid=<N> comm=<s> ring=0x<hex> wptr=0x<hex>
            //            rptr=0x<hex> size=<dec> node=<N>"
            if (line.find("QUEUE ") != 0)
                continue;

            auto get_u64 = [&](const char* key) -> uint64_t {
                std::string needle = std::string(" ") + key + "=";
                size_t p = line.find(needle);
                if (p == std::string::npos)
                    return 0;
                p += needle.size();
                return strtoull(line.c_str() + p, nullptr, 0);
            };
            auto get_str = [&](const char* key) -> std::string {
                std::string needle = std::string(" ") + key + "=";
                size_t p = line.find(needle);
                if (p == std::string::npos)
                    return {};
                p += needle.size();
                size_t end = line.find(' ', p);
                return line.substr(p, end == std::string::npos ? end : end - p);
            };

            int pid = static_cast<int>(get_u64("pid"));
            std::string comm = get_str("comm");
            uint64_t ring = get_u64("ring");
            uint64_t wptr = get_u64("wptr");
            uint64_t rptr = get_u64("rptr");
            uint64_t size = get_u64("size");
            uint32_t node = static_cast<uint32_t>(get_u64("node"));

            if (!ring || !size)
                continue;

            // pid from bpftrace is the host-level PID; pid_filter_ is the WSL2
            // namespace PID. We cannot compare them directly — PID selection is
            // handled below when we compute ns_pid.

            // Prefer /proc/<pid>/comm for the authoritative name.
            // bpftrace reports the host-level PID, which differs from the WSL2
            // namespace PID by a fixed offset (~93000 on this system).
            // process_vm_readv and /proc/<pid>/ use the WSL2 namespace PID.
            int ns_pid = pid; // fallback: use bpftrace host PID
            if (pid_filter_) {
                // Verify the event plausibly belongs to our target by comparing
                // the bpftrace-reported comm against /proc/<pid_filter_>/comm.
                // This guards against misattributing queues from other
                // processes that happen to fire while we're watching.
                char tpath[64];
                snprintf(tpath, sizeof(tpath), "/proc/%d/comm", pid_filter_);
                std::ifstream tf(tpath);
                std::string target_comm;
                if (tf && std::getline(tf, target_comm) &&
                    !target_comm.empty() &&
                    comm.find(target_comm.substr(0, 15)) == std::string::npos &&
                    target_comm.find(comm.substr(0, 15)) == std::string::npos) {
                    continue; // comm mismatch — skip this event
                }
                ns_pid = pid_filter_;
            } else {
                // Best-effort heuristic: scan /proc for a process with the same
                // comm name within a plausible PID offset range.
                for (int candidate = pid - 200000; candidate <= pid;
                     candidate += 1) {
                    if (candidate <= 0)
                        continue;
                    char cpath2[64];
                    snprintf(cpath2, sizeof(cpath2), "/proc/%d/comm",
                             candidate);
                    std::ifstream cf2(cpath2);
                    std::string c2;
                    if (cf2 && std::getline(cf2, c2) && !c2.empty()) {
                        if (c2.find(comm.substr(0, 15)) != std::string::npos) {
                            ns_pid = candidate;
                            break;
                        }
                    }
                }
            }

            char cpath[64];
            snprintf(cpath, sizeof(cpath), "/proc/%d/comm", ns_pid);
            std::ifstream cf(cpath);
            std::string real_comm;
            if (cf && std::getline(cf, real_comm) && !real_comm.empty())
                comm = real_comm;

            QueueInfo q;
            q.pid = ns_pid;
            q.comm = comm;
            q.ring_base = ring;
            q.wptr_addr = wptr;
            q.rptr_addr = rptr;
            q.ring_size = static_cast<uint32_t>(size);
            q.gpu_id = node;
            // librocdxg uses HSA_QUEUE_COMPUTE_AQL = 21; is_aql() handles both.
            q.qtype = 21;
            q.create_ts = MonoNow();
            q.uid = next_uid++;

            if (cb_)
                cb_(q);
        }
    }
}

} // namespace hsasnoop
