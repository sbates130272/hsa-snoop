// hsa-snoop: detect HSA AQL queues via the amdgpu/KFD driver and trace the
// packets flowing across them, emitting a Perfetto (or Chrome JSON) trace.
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
  fprintf(stderr,
    "hsa-snoop - snoop HSA AQL queue activity via the amdgpu driver\n\n"
    "Usage:\n"
    "  sudo %s [options] --pid <pid>        # attach to a running process\n"
    "  sudo %s [options] -- <command...>    # launch and trace a command\n\n"
    "Options:\n"
    "  --pid <pid>        Trace only this process\n"
    "  --out <file>       Output trace path\n"
    "                     (default hsa-snoop.pftrace / .json)\n"
    "  --format <fmt>     perfetto (default) | json\n"
    "  --poll-us <n>      Ring poll interval in microseconds (default 20)\n"
    "  --duration <sec>   Auto-stop after N seconds (0 = until Ctrl-C/exit)\n"
    "  --tracefs <path>   tracefs mount (default /sys/kernel/tracing)\n",
    p, p);
}
}  // namespace

int main(int argc, char** argv) {
  int pid_filter = 0, poll_us = 20, duration = 0;
  std::string out, tracefs = "/sys/kernel/tracing";
  Format fmt = Format::kPerfetto;
  std::vector<char*> child_cmd;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--pid" && i + 1 < argc) pid_filter = atoi(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--poll-us" && i + 1 < argc) poll_us = atoi(argv[++i]);
    else if (a == "--duration" && i + 1 < argc) duration = atoi(argv[++i]);
    else if (a == "--tracefs" && i + 1 < argc) tracefs = argv[++i];
    else if (a == "--format" && i + 1 < argc)
      fmt = std::string(argv[++i]) == "json" ? Format::kJson : Format::kPerfetto;
    else if (a == "--") { for (int j = i + 1; j < argc; ++j) child_cmd.push_back(argv[j]); break; }
    else if (a == "-h" || a == "--help") { Usage(argv[0]); return 0; }
    else { fprintf(stderr, "unknown arg: %s\n", a.c_str()); Usage(argv[0]); return 2; }
  }

  if (geteuid() != 0) {
    fprintf(stderr, "hsa-snoop: must run as root (needs tracefs, pagemap, "
                    "process_vm_readv). Try sudo.\n");
    return 1;
  }
  if (!pid_filter && child_cmd.empty()) {
    fprintf(stderr, "hsa-snoop: specify --pid or a command after --\n");
    Usage(argv[0]);
    return 2;
  }
  if (out.empty())
    out = fmt == Format::kJson ? "hsa-snoop.json" : "hsa-snoop.pftrace";

  signal(SIGINT, OnSignal);
  signal(SIGTERM, OnSignal);

  TraceWriter writer(fmt);

  RingParser parser([&writer](const PacketRecord& r) { writer.Add(r); }, poll_us);

  int queue_count = 0;
  Discovery discovery(tracefs, pid_filter);
  auto on_queue = [&](const QueueInfo& q_in) {
    QueueInfo q = q_in;
    if (!q.is_aql()) return;  // only host<->GPU AQL queues
    q.ring_phys = VirtToPhys(q.pid, q.ring_base);
    writer.RegisterQueue(q);
    parser.AddQueue(q);
    ++queue_count;
    fprintf(stderr,
      "[queue] pid=%d comm=%s uid=%lu ring_va=0x%lx ring_phys=0x%lx "
      "size=%uB slots=%u gpu=%u\n",
      q.pid, q.comm.c_str(), q.uid, q.ring_base, q.ring_phys, q.ring_size,
      q.num_slots(), q.gpu_id);
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
    child = fork();
    if (child == 0) {
      execvp(child_cmd[0], child_cmd.data());
      perror("execvp");
      _exit(127);
    }
    pid_filter = child;  // (discovery already filters; informational)
    fprintf(stderr, "hsa-snoop: launched '%s' pid=%d\n", child_cmd[0], child);
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
      if ((ts.tv_sec + ts.tv_nsec * 1e-9) - t0 >= duration) break;
    }
  }

  discovery.Stop();
  parser.Stop();

  fprintf(stderr, "hsa-snoop: %d AQL queue(s), %zu packet(s) captured.\n",
          queue_count, writer.count());
  if (!writer.Write(out)) {
    fprintf(stderr, "hsa-snoop: failed to write %s\n", out.c_str());
    return 1;
  }
  fprintf(stderr, "hsa-snoop: wrote %s (open in https://ui.perfetto.dev)\n",
          out.c_str());
  return 0;
}
