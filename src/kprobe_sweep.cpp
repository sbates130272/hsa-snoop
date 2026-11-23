#include "kprobe_sweep.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

namespace hsasnoop {
namespace {

constexpr char kAisInstancePrefix[] = "hsa_ais_";

bool WriteStr(const std::string& path, const std::string& val, bool append) {
    int fd = open(path.c_str(), O_WRONLY | (append ? O_APPEND : O_TRUNC));
    if (fd < 0)
        return false;
    ssize_t n = write(fd, val.data(), val.size());
    close(fd);
    return n == static_cast<ssize_t>(val.size());
}

// Parse a string that is nothing but a positive decimal pid.
bool ParsePid(const std::string& s, int* pid) {
    if (s.empty() || s.size() > 9)
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    *pid = atoi(s.c_str());
    return *pid > 0;
}

std::vector<std::string> ListDir(const std::string& path) {
    std::vector<std::string> names;
    DIR* d = opendir(path.c_str());
    if (!d)
        return names;
    while (struct dirent* e = readdir(d)) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            names.emplace_back(e->d_name);
    }
    closedir(d);
    return names;
}

} // namespace

bool ParseOwnedProbe(const std::string& line, std::string* name, int* pid) {
    // "p:kprobes/hsasnoop_cq_1234 kfd_ioctl_create_queue ring_base=..."
    // "r1024:kprobes/hsasnoop_ais_r_1234 kfd_ioctl_ais ret=$retval:s32"
    if (line.empty() || (line[0] != 'p' && line[0] != 'r'))
        return false;
    size_t colon = line.find(':');
    size_t end = line.find_first_of(" \t\n", 0);
    if (end == std::string::npos)
        end = line.size();
    if (colon == std::string::npos || colon > end)
        return false;
    std::string event = line.substr(colon + 1, end - colon - 1);
    size_t slash = event.find('/');
    if (slash != std::string::npos) {
        if (event.compare(0, slash, "kprobes") != 0)
            return false;
        event.erase(0, slash + 1);
    }
    for (const char* prefix :
         {"hsasnoop_cq_", "hsasnoop_ais_e_", "hsasnoop_ais_r_"}) {
        const size_t n = strlen(prefix);
        if (event.compare(0, n, prefix) != 0)
            continue;
        if (!ParsePid(event.substr(n), pid))
            return false;
        *name = event;
        return true;
    }
    return false;
}

bool IsLiveHsaSnoop(int pid, const std::string& procfs) {
    const std::string dir = procfs + "/" + std::to_string(pid);
    std::string comm;
    {
        std::ifstream f(dir + "/comm");
        if (!f || !std::getline(f, comm))
            return false;
    }
    // A zombie keeps its name but has released its probes' only remover.
    {
        std::ifstream f(dir + "/stat");
        std::string stat;
        std::getline(f, stat);
        size_t paren = stat.rfind(')');
        if (paren != std::string::npos && paren + 2 < stat.size() &&
            (stat[paren + 2] == 'Z' || stat[paren + 2] == 'X'))
            return false;
    }
    if (comm == "hsa-snoop")
        return true;
    // The binary may have been installed under another name.
    std::string self;
    std::ifstream f(procfs + "/self/comm");
    return f && std::getline(f, self) && comm == self;
}

int SweepStaleProbes(const std::string& tracefs, const std::string& procfs) {
    struct Stale {
        std::string name;
        int pid;
    };
    std::vector<Stale> stale;
    {
        std::ifstream f(tracefs + "/kprobe_events");
        std::string line;
        while (std::getline(f, line)) {
            Stale s;
            if (ParseOwnedProbe(line, &s.name, &s.pid) && s.pid != getpid() &&
                !IsLiveHsaSnoop(s.pid, procfs))
                stale.push_back(std::move(s));
        }
    }

    const std::string instances = tracefs + "/instances";
    const std::vector<std::string> instance_names = ListDir(instances);

    int removed = 0;
    for (const Stale& s : stale) {
        // An enabled probe cannot be removed. The discovery probe is enabled
        // in the top-level instance, the AIS probes in an instance of their
        // own; turn it off wherever it is.
        const std::string enable = "/events/kprobes/" + s.name + "/enable";
        WriteStr(tracefs + enable, "0\n", false);
        for (const std::string& inst : instance_names)
            WriteStr(instances + "/" + inst + enable, "0\n", false);
        if (WriteStr(tracefs + "/kprobe_events", "-:" + s.name + "\n", true)) {
            fprintf(stderr,
                    "hsa-snoop: removed stale kprobe %s (pid %d is not a "
                    "running hsa-snoop)\n",
                    s.name.c_str(), s.pid);
            ++removed;
        } else {
            fprintf(stderr, "hsa-snoop: could not remove stale kprobe %s: %s\n",
                    s.name.c_str(), strerror(errno));
        }
    }

    for (const std::string& inst : instance_names) {
        int pid = 0;
        const size_t n = sizeof(kAisInstancePrefix) - 1;
        if (inst.compare(0, n, kAisInstancePrefix) != 0 ||
            !ParsePid(inst.substr(n), &pid) || pid == getpid() ||
            IsLiveHsaSnoop(pid, procfs))
            continue;
        const std::string path = instances + "/" + inst;
        if (rmdir(path.c_str()) == 0)
            fprintf(stderr, "hsa-snoop: removed stale tracefs instance %s\n",
                    path.c_str());
        else if (errno != ENOENT)
            fprintf(stderr,
                    "hsa-snoop: could not remove stale tracefs instance %s: "
                    "%s\n",
                    path.c_str(), strerror(errno));
    }
    return removed;
}

} // namespace hsasnoop
