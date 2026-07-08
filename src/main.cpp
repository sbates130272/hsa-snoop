// hsa-snoop: detect HSA AQL queues via the amdgpu/KFD driver and trace the
// packets flowing across them, emitting a Perfetto (or Chrome JSON) trace.
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "discovery.h"
#include "parser.h"
#include "proc_mem.h"
#include "trace_writer.h"

using namespace hsasnoop;

namespace {
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

void Usage(const char* p) {
    fprintf(
        stderr,
        "hsa-snoop - snoop HSA AQL queue activity via the amdgpu driver\n\n"
        "Usage:\n"
        "  sudo %s [options] --pid <pid>        # attach to a running process\n"
        "  sudo %s [options] -- <command...>    # launch and trace a "
        "command\n"
        "  sudo %s [options] --all              # monitor all GPU processes "
        "(daemon mode)\n\n"
        "Options:\n"
        "  --pid <pid>        Trace only this process\n"
        "  --all              Monitor all HSA/HIP processes system-wide;\n"
        "                     writes a new trace per process to --out-dir\n"
        "  --out <file>       Output trace path (single-process modes)\n"
        "                     (default hsa-snoop.pftrace / .json)\n"
        "  --out-dir <dir>    Output directory for --all mode\n"
        "                     (default /var/log/hsa-snoop)\n"
        "  --format <fmt>     perfetto (default) | json\n"
        "  --poll-us <n>      Ring poll interval in microseconds (default 20)\n"
        "  --duration <sec>   Auto-stop after N seconds (0 = until "
        "Ctrl-C/exit)\n"
        "  --tracefs <path>   tracefs mount (default /sys/kernel/tracing)\n"
        "  --mode <mode>      kprobe (default) | bpftrace\n"
        "                     bpftrace: for WSL2/librocdxg systems without "
        "KFD\n"
        "                     auto-selected when running in WSL2\n"
        "  --librocdxg <path> Path to librocdxg.so.* (bpftrace mode; "
        "auto-detected)\n",
        p, p, p);
}
} // namespace

int main(int argc, char** argv) {
    int pid_filter = 0, poll_us = 20, duration = 0;
    bool all_mode = false;
    std::string out, out_dir, tracefs = "/sys/kernel/tracing";
    std::string mode_str, librocdxg_path;
    Format fmt = Format::kPerfetto;
    std::vector<char*> child_cmd;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pid" && i + 1 < argc)
            pid_filter = atoi(argv[++i]);
        else if (a == "--all")
            all_mode = true;
        else if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--out-dir" && i + 1 < argc)
            out_dir = argv[++i];
        else if (a == "--poll-us" && i + 1 < argc)
            poll_us = atoi(argv[++i]);
        else if (a == "--duration" && i + 1 < argc)
            duration = atoi(argv[++i]);
        else if (a == "--tracefs" && i + 1 < argc)
            tracefs = argv[++i];
        else if (a == "--mode" && i + 1 < argc)
            mode_str = argv[++i];
        else if (a == "--librocdxg" && i + 1 < argc)
            librocdxg_path = argv[++i];
        else if (a == "--format" && i + 1 < argc)
            fmt = std::string(argv[++i]) == "json" ? Format::kJson
                                                   : Format::kPerfetto;
        else if (a == "--") {
            for (int j = i + 1; j < argc; ++j)
                child_cmd.push_back(argv[j]);
            break;
        } else if (a == "-h" || a == "--help") {
            Usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", a.c_str());
            Usage(argv[0]);
            return 2;
        }
    }

    // Determine discovery mode: explicit --mode overrides, otherwise
    // auto-detect.
    DiscoveryMode disc_mode = DiscoveryMode::kKprobe;
    if (mode_str == "bpftrace") {
        disc_mode = DiscoveryMode::kBpftrace;
    } else if (mode_str.empty() && IsWsl2()) {
        disc_mode = DiscoveryMode::kBpftrace;
        fprintf(stderr,
                "hsa-snoop: WSL2 detected, switching to bpftrace mode\n");
    } else if (!mode_str.empty() && mode_str != "kprobe") {
        fprintf(stderr, "hsa-snoop: unknown --mode '%s' (kprobe|bpftrace)\n",
                mode_str.c_str());
        return 2;
    }

    // In bpftrace mode, find librocdxg if not supplied explicitly.
    if (disc_mode == DiscoveryMode::kBpftrace && librocdxg_path.empty()) {
        librocdxg_path = FindLibrocdxg();
        if (librocdxg_path.empty()) {
            fprintf(stderr, "hsa-snoop: cannot locate librocdxg.so; "
                            "specify --librocdxg <path>\n");
            return 1;
        }
        fprintf(stderr, "hsa-snoop: using librocdxg at %s\n",
                librocdxg_path.c_str());
    }

    if (geteuid() != 0) {
        fprintf(stderr, "hsa-snoop: must run as root (needs tracefs, pagemap, "
                        "process_vm_readv). Try sudo.\n");
        return 1;
    }
    if (!all_mode && !pid_filter && child_cmd.empty()) {
        fprintf(stderr,
                "hsa-snoop: specify --pid, a command after --, or --all\n");
        Usage(argv[0]);
        return 2;
    }
    if (all_mode && (pid_filter || !child_cmd.empty())) {
        fprintf(stderr,
                "hsa-snoop: --all is mutually exclusive with --pid and --\n");
        return 2;
    }

    // In --all mode each queue's trace is written to <out-dir>/<comm>-<uid>.ext
    // so that concurrent workloads don't clobber each other.
    if (all_mode) {
        if (out_dir.empty())
            out_dir = "/var/log/hsa-snoop";
        fprintf(stderr, "hsa-snoop: --all mode, traces → %s/\n",
                out_dir.c_str());
    } else if (out.empty()) {
        out = fmt == Format::kJson ? "hsa-snoop.json" : "hsa-snoop.pftrace";
    }

    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    // In --all mode we keep one TraceWriter per discovered queue so that each
    // process gets its own output file. In single-process modes one shared
    // writer covers all queues belonging to the target.
    std::unique_ptr<TraceWriter> single_writer;
    std::map<uint64_t, std::unique_ptr<TraceWriter>> per_queue_writers;
    std::mutex writers_mu;

    auto get_writer = [&](const QueueInfo& q) -> TraceWriter& {
        if (!all_mode)
            return *single_writer;
        std::lock_guard<std::mutex> lk(writers_mu);
        auto it = per_queue_writers.find(q.uid);
        if (it != per_queue_writers.end())
            return *it->second;
        // Build <out-dir>/<comm>-<uid>.<ext>
        std::string ext = fmt == Format::kJson ? ".json" : ".pftrace";
        std::string path =
            out_dir + "/" + q.comm + "-" + std::to_string(q.uid) + ext;
        auto w = std::make_unique<TraceWriter>(fmt);
        w->RegisterQueue(q);
        per_queue_writers[q.uid] = std::move(w);
        fprintf(stderr, "hsa-snoop: new trace → %s\n", path.c_str());
        return *per_queue_writers[q.uid];
    };

    if (!all_mode)
        single_writer = std::make_unique<TraceWriter>(fmt);

    RingParser parser(
        [&](const PacketRecord& r) {
            if (all_mode) {
                std::lock_guard<std::mutex> lk(writers_mu);
                auto it = per_queue_writers.find(r.queue_uid);
                if (it != per_queue_writers.end())
                    it->second->Add(r);
            } else {
                single_writer->Add(r);
            }
        },
        poll_us);

    int queue_count = 0;
    Discovery discovery(tracefs, pid_filter, disc_mode, librocdxg_path);
    auto on_queue = [&](const QueueInfo& q_in) {
        QueueInfo q = q_in;
        if (!q.is_aql())
            return; // only host<->GPU AQL queues
        q.ring_phys = VirtToPhys(q.pid, q.ring_base);
        TraceWriter& w = get_writer(q);
        if (!all_mode)
            w.RegisterQueue(q); // in all_mode RegisterQueue called in get_writer
        parser.AddQueue(q);
        ++queue_count;
        fprintf(stderr,
                "[queue] pid=%d comm=%s uid=%lu ring_va=0x%lx ring_phys=0x%lx "
                "size=%uB slots=%u gpu=%u\n",
                q.pid, q.comm.c_str(), q.uid, q.ring_base, q.ring_phys,
                q.ring_size, q.num_slots(), q.gpu_id);
    };

    if (!discovery.Start(on_queue)) {
        fprintf(stderr, "hsa-snoop: failed to start discovery\n");
        return 1;
    }
    fprintf(stderr, "hsa-snoop: discovery armed. Watching for AQL queues...\n");

    // Launch mode: fork/exec the target now that the probe is armed.
    pid_t child = -1;
    if (!child_cmd.empty()) {
        child_cmd.push_back(nullptr);

        // In bpftrace/WSL2 mode, the GPU is only accessible to the session
        // user, not to root. When hsa-snoop runs via `sudo`, we must drop back
        // to the invoking user so the child process can access the GPU.
        // SUDO_UID/SUDO_GID/SUDO_USER are set by sudo and identify the original
        // user.
        uid_t run_uid = geteuid();
        gid_t run_gid = getegid();
        const char* sudo_uid_str = getenv("SUDO_UID");
        const char* sudo_gid_str = getenv("SUDO_GID");
        bool drop_privs = disc_mode == DiscoveryMode::kBpftrace &&
                          sudo_uid_str && sudo_gid_str;
        if (drop_privs) {
            run_uid = (uid_t)atoi(sudo_uid_str);
            run_gid = (gid_t)atoi(sudo_gid_str);
        }

        child = fork();
        if (child == 0) {
            if (disc_mode == DiscoveryMode::kBpftrace) {
                // Ensure the ROCm library path is set (sudo strips
                // LD_LIBRARY_PATH).
                if (getenv("LD_LIBRARY_PATH") == nullptr)
                    setenv("LD_LIBRARY_PATH", "/opt/rocm/lib:/usr/lib/wsl/lib",
                           0);
                // HSA_ENABLE_DXG_DETECTION tells the HSA runtime to use the
                // WSL2/librocdxg DXG backend. Without it the GPU is not
                // accessible.
                setenv("HSA_ENABLE_DXG_DETECTION", "1", 0);
                // XDG_RUNTIME_DIR is needed for the GPU agent socket.
                if (getenv("XDG_RUNTIME_DIR") == nullptr && run_uid != 0) {
                    char xdg[64];
                    snprintf(xdg, sizeof(xdg), "/run/user/%u", run_uid);
                    setenv("XDG_RUNTIME_DIR", xdg, 0);
                }
            }
            if (drop_privs) {
                // Drop to the invoking user so the GPU is accessible in WSL2.
                const char* sudo_user = getenv("SUDO_USER");
                if (sudo_user && initgroups(sudo_user, run_gid) < 0)
                    setgroups(0, nullptr);
                if (setgid(run_gid) < 0 || setuid(run_uid) < 0) {
                    perror("hsa-snoop: drop privileges");
                    _exit(1);
                }
            }
            execvp(child_cmd[0], child_cmd.data());
            perror("execvp");
            _exit(127);
        }
        pid_filter = child; // (discovery already filters; informational)
        fprintf(stderr, "hsa-snoop: launched '%s' pid=%d%s\n", child_cmd[0],
                child, drop_privs ? " (as original user)" : "");
    }

    double t0 = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    t0 = ts.tv_sec + ts.tv_nsec * 1e-9;

    while (!g_stop) {
        usleep(50 * 1000);
        if (child > 0) {
            int status;
            pid_t r = waitpid(child, &status, WNOHANG);
            if (r == child) {
                // Give the parser a moment to drain remaining completions.
                usleep(200 * 1000);
                break;
            }
        }
        if (duration > 0) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            if ((ts.tv_sec + ts.tv_nsec * 1e-9) - t0 >= duration)
                break;
        }
    }

    discovery.Stop();
    parser.Stop();

    if (all_mode) {
        std::lock_guard<std::mutex> lk(writers_mu);
        size_t total_packets = 0;
        int failures = 0;
        std::string ext = fmt == Format::kJson ? ".json" : ".pftrace";
        for (auto& [uid, w] : per_queue_writers) {
            total_packets += w->count();
            // Reconstruct the path to report it.
            // (We stored the writer by uid; comm is not available here so we
            // derive the path the same way get_writer() did.)
            std::string path = out_dir + "/queue-" + std::to_string(uid) + ext;
            if (!w->Write(path)) {
                fprintf(stderr, "hsa-snoop: failed to write %s\n",
                        path.c_str());
                ++failures;
            } else {
                fprintf(stderr, "hsa-snoop: wrote %s\n", path.c_str());
            }
        }
        fprintf(stderr,
                "hsa-snoop: %d AQL queue(s), %zu packet(s) captured.\n",
                queue_count, total_packets);
        return failures ? 1 : 0;
    }

    fprintf(stderr, "hsa-snoop: %d AQL queue(s), %zu packet(s) captured.\n",
            queue_count, single_writer->count());
    if (!single_writer->Write(out)) {
        fprintf(stderr, "hsa-snoop: failed to write %s\n", out.c_str());
        return 1;
    }
    fprintf(stderr, "hsa-snoop: wrote %s (open in https://ui.perfetto.dev)\n",
            out.c_str());
    return 0;
}
