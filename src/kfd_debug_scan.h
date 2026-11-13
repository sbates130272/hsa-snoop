// kfd_debug_scan.h - Retroactive discovery of pre-existing KFD queues via the
// KFD debug-trap API (AMDKFD_IOC_DBG_TRAP / KFD_IOC_DBG_TRAP_GET_QUEUE_SNAPSHOT).
#pragma once
#include "model.h"
#include <functional>
#include <sys/types.h>

namespace hsasnoop {

// Scan all running processes that have /dev/kfd open and invoke cb for each
// queue found. Requires CAP_SYS_PTRACE; processes where ptrace or ioctl fails
// are silently skipped. pid_filter=0 scans all KFD processes; nonzero restricts
// to that single PID. Returns the number of processes successfully snapshotted.
//
// Protocol per process:
//   ptrace(PTRACE_SEIZE) → ENABLE → GET_QUEUE_SNAPSHOT → DISABLE → DETACH
// The target keeps running throughout; the debug session window is milliseconds.
int ScanExistingKfdQueues(std::function<void(const QueueInfo&)> cb,
                          pid_t pid_filter = 0);

} // namespace hsasnoop
