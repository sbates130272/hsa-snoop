// kprobe_sweep.h - remove tracefs probes left behind by a dead hsa-snoop.
//
// Probes are named after the pid that installed them. One killed with SIGKILL
// cannot remove its own, and a later hsa-snoop has a different pid, so
// nothing else ever would.
#pragma once

#include <string>

namespace hsasnoop {

// Parse one line of tracefs kprobe_events. Returns true if it defines a probe
// named by hsa-snoop (hsasnoop_cq_<pid>, hsasnoop_ais_e_<pid>,
// hsasnoop_ais_r_<pid>), with the event name and the owning pid.
bool ParseOwnedProbe(const std::string& line, std::string* name, int* pid);

// True if `pid` is a running hsa-snoop according to `procfs`. A pid that has
// been reused by another program does not count.
bool IsLiveHsaSnoop(int pid, const std::string& procfs = "/proc");

// Remove every hsa-snoop probe and AIS tracefs instance whose owner is not a
// running hsa-snoop. Returns the number of probes removed.
int SweepStaleProbes(const std::string& tracefs,
                     const std::string& procfs = "/proc");

} // namespace hsasnoop
