// aql.h - Thin, self-contained mirror of the HSA AQL packet formats.
//
// We deliberately do NOT #include <hsa/hsa.h> here so that hsa-snoop can be
// built on a machine that only ships the ROCr runtime (no -dev headers). The
// layouts below are byte-for-byte compatible with the structures in
// /opt/rocm/include/hsa/hsa.h and /opt/rocm/include/hsa/amd_hsa_queue.h and are
// verified against them in the README. Every AQL packet is exactly 64 bytes.
#pragma once
#include <cstdint>

namespace aql {

constexpr uint32_t kPacketSize = 64; // All AQL packets are 64 bytes.

// hsa_packet_type_t
enum class PacketType : uint8_t {
    VendorSpecific = 0,
    Invalid = 1,
    KernelDispatch = 2,
    BarrierAnd = 3,
    AgentDispatch = 4,
    BarrierOr = 5,
};

const char* PacketTypeName(PacketType t);

// Sub-fields of the 16-bit AQL packet header (hsa_packet_header_t).
constexpr int kHeaderTypeShift = 0;     // width 8
constexpr int kHeaderBarrierShift = 8;  // width 1
constexpr int kHeaderAcquireShift = 9;  // width 2
constexpr int kHeaderReleaseShift = 11; // width 2

inline PacketType HeaderType(uint16_t header) {
    return static_cast<PacketType>((header >> kHeaderTypeShift) & 0xFF);
}
inline bool HeaderBarrier(uint16_t header) {
    return (header >> kHeaderBarrierShift) & 0x1;
}

// hsa_kernel_dispatch_packet_t (64 bytes). Matches hsa.h exactly.
#pragma pack(push, 1)
struct KernelDispatchPacket {
    uint16_t header;
    uint16_t setup; // low 2 bits = grid dimensions
    uint16_t workgroup_size_x;
    uint16_t workgroup_size_y;
    uint16_t workgroup_size_z;
    uint16_t reserved0;
    uint32_t grid_size_x;
    uint32_t grid_size_y;
    uint32_t grid_size_z;
    uint32_t private_segment_size;
    uint32_t group_segment_size;
    uint64_t kernel_object;   // address of the kernel descriptor (KD)
    uint64_t kernarg_address; // HSA_LARGE_MODEL (64-bit) layout
    uint64_t reserved2;
    uint64_t completion_signal; // hsa_signal_t handle (0 == none)
};
static_assert(sizeof(KernelDispatchPacket) == 64, "AQL dispatch must be 64B");

// hsa_barrier_and_packet_t / hsa_barrier_or_packet_t (identical layout).
struct BarrierPacket {
    uint16_t header;
    uint16_t reserved0;
    uint32_t reserved1;
    uint64_t dep_signal[5];
    uint64_t reserved2;
    uint64_t completion_signal;
};
static_assert(sizeof(BarrierPacket) == 64, "AQL barrier must be 64B");

// hsa_agent_dispatch_packet_t.
struct AgentDispatchPacket {
    uint16_t header;
    uint16_t type;
    uint32_t reserved0;
    uint64_t return_address;
    uint64_t arg[4];
    uint64_t reserved2;
    uint64_t completion_signal;
};
static_assert(sizeof(AgentDispatchPacket) == 64, "AQL agent must be 64B");
#pragma pack(pop)

inline uint32_t GridDims(uint16_t setup) { return setup & 0x3; }

} // namespace aql
