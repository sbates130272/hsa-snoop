// model.h - Core data model shared across hsa-snoop components.
#pragma once
#include "aql.h"
#include <cstdint>
#include <string>

namespace hsasnoop {

// A single HSA AQL queue discovered via the amdgpu/KFD create-queue ioctl.
struct QueueInfo {
    int pid = 0;
    std::string comm;
    uint64_t ring_base = 0; // user VA of the AQL ring buffer (in pid)
    uint64_t wptr_addr = 0; // user VA of write_dispatch_id (u64)
    uint64_t rptr_addr = 0; // user VA of read_dispatch_id  (u64)
    uint32_t ring_size = 0; // ring buffer size in bytes
    uint32_t gpu_id = 0;    // KFD gpu_id
    uint32_t qtype = 0;     // KFD_IOC_QUEUE_TYPE_*
    double create_ts = 0;   // host CLOCK_MONOTONIC_RAW seconds

    // Physical address of the first ring page (from /proc/pid/pagemap), or 0.
    uint64_t ring_phys = 0;

    // Stable per-run identifier used as the Perfetto track id.
    uint64_t uid = 0;

    uint32_t num_slots() const {
        return ring_size ? ring_size / aql::kPacketSize : 0;
    }
    bool is_aql() const {
        return qtype == 2; /* KFD_IOC_QUEUE_TYPE_COMPUTE_AQL */
    }
};

// One observed packet on a queue, with timing.
struct PacketRecord {
    uint64_t queue_uid = 0;
    uint64_t dispatch_id = 0; // monotonic AQL packet index
    aql::PacketType type = aql::PacketType::Invalid;
    bool barrier = false;

    // Kernel dispatch fields (valid when type == KernelDispatch).
    uint64_t kernel_object = 0;
    std::string kernel_name;
    uint32_t grid[3] = {0, 0, 0};
    uint16_t wg[3] = {0, 0, 0};
    uint32_t private_seg = 0;
    uint32_t group_seg = 0;
    uint64_t completion_signal = 0;
    uint64_t kernarg_address = 0;

    // Timing (host CLOCK_MONOTONIC_RAW seconds).
    double submit_ts = 0;   // first observed pending (id < wptr, id >= rptr)
    double complete_ts = 0; // observed after rptr advanced past id
    bool completed = false;
};

} // namespace hsasnoop
