// Unit tests for the startup sweep of probes left behind by a dead hsa-snoop.
// The name parsing is tested directly; the sweep runs against a fake tracefs
// and procfs built from plain files, so it needs no root and no kernel
// support.

#include "kprobe_sweep.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace hsasnoop;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void WriteFile(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

std::string ReadFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool Exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

void MakeDirs(const std::string& path) {
    std::string cmd = "mkdir -p '" + path + "'";
    Check(std::system(cmd.c_str()) == 0, "mkdir -p");
}

void FakeProcess(const std::string& procfs, int pid, const std::string& comm,
                 char state) {
    const std::string dir = procfs + "/" + std::to_string(pid);
    MakeDirs(dir);
    WriteFile(dir + "/comm", comm + "\n");
    WriteFile(dir + "/stat", std::to_string(pid) + " (" + comm + ") " + state +
                                 " 1 1 1 0 -1 4194560\n");
}

void TestParse() {
    std::string name;
    int pid = 0;

    Check(ParseOwnedProbe("p:kprobes/hsasnoop_cq_4242 kfd_ioctl_create_queue "
                          "ring_base=+0(%dx):u64 wptr=+8(%dx):u64",
                          &name, &pid) &&
              name == "hsasnoop_cq_4242" && pid == 4242,
          "discovery probe");
    Check(ParseOwnedProbe("p:kprobes/hsasnoop_ais_e_17 kfd_ioctl_ais "
                          "gpu_id=+4(%dx):u32",
                          &name, &pid) &&
              name == "hsasnoop_ais_e_17" && pid == 17,
          "AIS entry probe");
    Check(ParseOwnedProbe("r1024:kprobes/hsasnoop_ais_r_17 kfd_ioctl_ais "
                          "ret=$retval:s32",
                          &name, &pid) &&
              name == "hsasnoop_ais_r_17" && pid == 17,
          "AIS return probe with a maxactive count");
    Check(ParseOwnedProbe("p:hsasnoop_cq_9 kfd_ioctl_create_queue", &name,
                          &pid) &&
              name == "hsasnoop_cq_9" && pid == 9,
          "probe with no group");

    // Other tools' probes, and names that only look like ours, are left alone.
    Check(!ParseOwnedProbe("p:kprobes/myprobe do_sys_open", &name, &pid),
          "foreign probe");
    Check(!ParseOwnedProbe("p:kprobes/hsasnoop_cq_ kfd_ioctl_create_queue",
                           &name, &pid),
          "no pid");
    Check(!ParseOwnedProbe("p:kprobes/hsasnoop_cq_12ab kfd_ioctl_create_queue",
                           &name, &pid),
          "pid with trailing text");
    Check(!ParseOwnedProbe("p:kprobes/hsasnoop_cq_0 kfd_ioctl_create_queue",
                           &name, &pid),
          "pid zero");
    Check(!ParseOwnedProbe("p:kprobes/hsasnoop_cq_-5 kfd_ioctl_create_queue",
                           &name, &pid),
          "negative pid");
    Check(!ParseOwnedProbe("p:kprobes/hsasnoop_other_5 kfd_ioctl_ais", &name,
                           &pid),
          "unknown hsasnoop probe kind");
    Check(
        !ParseOwnedProbe("p:kprobes/xhsasnoop_cq_5 kfd_ioctl_ais", &name, &pid),
        "prefix must start the name");
    Check(!ParseOwnedProbe("p:other/hsasnoop_cq_5 kfd_ioctl_ais", &name, &pid),
          "probe in another group");
    Check(!ParseOwnedProbe("p:kprobes/foo hsasnoop_cq_5", &name, &pid),
          "our name as the probed symbol");
    Check(!ParseOwnedProbe("", &name, &pid), "empty line");
    Check(!ParseOwnedProbe("-:hsasnoop_cq_5", &name, &pid), "removal command");
}

void TestSweep() {
    char tmpl[] = "/tmp/hsa-snoop-sweep-XXXXXX";
    const char* root = mkdtemp(tmpl);
    Check(root != nullptr, "mkdtemp");
    if (!root)
        return;
    const std::string tracefs = std::string(root) + "/tracing";
    const std::string procfs = std::string(root) + "/proc";

    // 100: gone. 200: a running hsa-snoop. 300: the pid now belongs to
    // another program. 400: an hsa-snoop that is a zombie.
    MakeDirs(procfs + "/self");
    WriteFile(procfs + "/self/comm", "kprobe-sweep-te\n");
    FakeProcess(procfs, 200, "hsa-snoop", 'S');
    FakeProcess(procfs, 300, "bash", 'S');
    FakeProcess(procfs, 400, "hsa-snoop", 'Z');

    Check(!IsLiveHsaSnoop(100, procfs), "missing pid is not live");
    Check(IsLiveHsaSnoop(200, procfs), "running hsa-snoop is live");
    Check(!IsLiveHsaSnoop(300, procfs), "reused pid is not an hsa-snoop");
    Check(!IsLiveHsaSnoop(400, procfs), "zombie hsa-snoop is not live");

    const std::string events =
        "p:kprobes/hsasnoop_cq_100 kfd_ioctl_create_queue ring_base=+0(%dx)\n"
        "p:kprobes/hsasnoop_cq_200 kfd_ioctl_create_queue ring_base=+0(%dx)\n"
        "p:kprobes/hsasnoop_ais_e_200 kfd_ioctl_ais gpu_id=+4(%dx):u32\n"
        "p:kprobes/hsasnoop_ais_e_300 kfd_ioctl_ais gpu_id=+4(%dx):u32\n"
        "r1024:kprobes/hsasnoop_ais_r_300 kfd_ioctl_ais ret=$retval:s32\n"
        "p:kprobes/hsasnoop_cq_400 kfd_ioctl_create_queue ring_base=+0(%dx)\n"
        "p:kprobes/someone_else do_sys_open\n";
    MakeDirs(tracefs);
    WriteFile(tracefs + "/kprobe_events", events);
    const std::string names[] = {"hsasnoop_cq_100",    "hsasnoop_cq_200",
                                 "hsasnoop_ais_e_200", "hsasnoop_ais_e_300",
                                 "hsasnoop_ais_r_300", "hsasnoop_cq_400",
                                 "someone_else"};
    for (const std::string& n : names) {
        MakeDirs(tracefs + "/events/kprobes/" + n);
        WriteFile(tracefs + "/events/kprobes/" + n + "/enable", "1\n");
    }
    // An instance of a live hsa-snoop, one of a dead one, and a stranger's.
    MakeDirs(tracefs + "/instances/hsa_ais_200");
    MakeDirs(tracefs + "/instances/hsa_ais_300");
    MakeDirs(tracefs + "/instances/other");

    Check(SweepStaleProbes(tracefs, procfs) == 4, "four stale probes removed");

    // The fake kprobe_events is a plain file, so removals show up appended.
    const std::string after = ReadFile(tracefs + "/kprobe_events");
    Check(after.compare(0, events.size(), events) == 0,
          "existing definitions untouched");
    Check(after.substr(events.size()) ==
              "-:hsasnoop_cq_100\n-:hsasnoop_ais_e_300\n"
              "-:hsasnoop_ais_r_300\n-:hsasnoop_cq_400\n",
          "exactly the stale probes are removed");

    auto enabled = [&](const std::string& n) {
        return ReadFile(tracefs + "/events/kprobes/" + n + "/enable") == "1\n";
    };
    Check(!enabled("hsasnoop_cq_100") && !enabled("hsasnoop_ais_e_300") &&
              !enabled("hsasnoop_ais_r_300") && !enabled("hsasnoop_cq_400"),
          "stale probes are disabled before removal");
    Check(enabled("hsasnoop_cq_200") && enabled("hsasnoop_ais_e_200") &&
              enabled("someone_else"),
          "probes of a live hsa-snoop and of others stay enabled");

    Check(Exists(tracefs + "/instances/hsa_ais_200"),
          "instance of a live hsa-snoop kept");
    Check(!Exists(tracefs + "/instances/hsa_ais_300"),
          "instance of a dead hsa-snoop removed");
    Check(Exists(tracefs + "/instances/other"), "foreign instance kept");

    // No tracefs at all is not an error.
    Check(SweepStaleProbes(std::string(root) + "/absent", procfs) == 0,
          "missing tracefs");

    std::string cmd = "rm -rf '" + std::string(root) + "'";
    Check(std::system(cmd.c_str()) == 0, "cleanup");
}

} // namespace

int main() {
    TestParse();
    TestSweep();
    if (failures) {
        std::fprintf(stderr, "%d kprobe sweep check(s) failed\n", failures);
        return 1;
    }
    std::fprintf(stderr, "PASS kprobe-sweep-test\n");
    return 0;
}
