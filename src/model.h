// model.h - Core data model shared across hsa-snoop components.
#pragma once
#include "aql.h"
#include "sdma.h"
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
    // Ring size expressed in dwords. SDMA rings carry variable-length
    // microcode packets decoded dword-by-dword, not in fixed 64-byte slots.
    uint32_t num_dwords() const { return ring_size / 4; }
    bool is_aql() const {
        // KFD_IOC_QUEUE_TYPE_COMPUTE_AQL == 2 (native amdgpu/KFD)
        // HSA_QUEUE_COMPUTE_AQL          == 21 (librocdxg / WSL2 WDDM path)
        return qtype == 2 || qtype == 21;
    }
    bool is_sdma() const {
        // KFD_IOC_QUEUE_TYPE_SDMA          == 1 (SDMA copy/DMA queue)
        // KFD_IOC_QUEUE_TYPE_SDMA_XGMI     == 3 (SDMA over XGMI to a peer GPU)
        // KFD_IOC_QUEUE_TYPE_SDMA_BY_ENG_ID== 4 (SDMA, engine chosen by id;
        //   this is what ROCm 6.x+/gfx942 actually uses for hipMemcpy copies --
        //   confirmed on MI300X, ROCm 7.2, kernel create_queue qtype=0x4)
        return qtype == 1 || qtype == 3 || qtype == 4;
    }
    // True for any queue kind hsa-snoop knows how to poll.
    bool is_traced() const { return is_aql() || is_sdma(); }
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

// Copy direction inferred from whether the src/dst pages are host- or
// device-backed (via /proc/<pid>/pagemap: a device/VRAM page has no PFN).
enum class CopyDir : uint8_t {
    Unknown = 0,
    HostToDevice,   // H2D  (system DRAM -> VRAM)
    DeviceToHost,   // D2H  (VRAM -> system DRAM)
    DeviceToDevice, // D2D  (VRAM -> VRAM, incl. XGMI peer)
    HostToHost,     // H2H  (system -> system)
};

const char* CopyDirName(CopyDir d);

// One observed packet on an SDMA queue, with timing. SDMA packets are the
// GPU's DMA-engine microcode (copies, fences, timestamps, ...) and are the
// path hipMemcpyAsync / peer DMA take -- invisible to the AQL compute snoop.
struct SdmaRecord {
    uint64_t queue_uid = 0;
    uint64_t seq = 0;       // monotonic packet index on this queue
    uint8_t opcode = 0;     // SDMA_OP_* (sdma::Op)
    uint8_t sub_opcode = 0; // opcode-specific sub-op (e.g. copy layout)
    std::string op_name;    // decoded op[/sub-op] name for display

    // Copy fields (valid when opcode == sdma::OP_COPY). bytes is the transfer
    // size; src/dst are the GPU-virtual (== process-virtual) addresses.
    uint64_t bytes = 0;
    uint64_t src_addr = 0;
    uint64_t dst_addr = 0;
    CopyDir dir = CopyDir::Unknown;

    // Timing (host CLOCK_MONOTONIC_RAW seconds).
    double submit_ts = 0;   // first observed enqueued (past rptr, before wptr)
    double complete_ts = 0; // observed after rptr advanced past the packet
    bool completed = false;

    bool is_copy() const { return opcode == sdma::OP_COPY; }
};

// Classification of a PCIe device backing an AIS IO target, derived from the
// PCI class code and a vendor-ID RDMA table. Used as Prometheus labels.
struct PcieDeviceInfo {
    std::string bdf;         // "0000:01:00.0"
    std::string device_type; // "nvme" | "rdma" | "unknown"
    std::string vendor; // human-readable vendor name, e.g. "NVIDIA / Mellanox"
    std::string vendor_id;  // raw hex VID, e.g. "15b3"
    std::string device_id;  // raw hex DID, e.g. "101e"
    std::string class_code; // raw hex class, e.g. "010802"
};

// IO direction for AIS (AMD Infinity Storage) operations. READ = file→VRAM,
// WRITE = VRAM→file. Mirrors enum kfd_ais_ops in kfd_ioctl.h.
enum class AisOp : uint8_t {
    Unknown = 0,
    Read = 1,  // KFD_IOC_AIS_READ  — file → VRAM (storage → GPU)
    Write = 2, // KFD_IOC_AIS_WRITE — VRAM → file (GPU → storage)
};

const char* AisOpName(AisOp op);

// One AIS (AMD Infinity Storage) ioctl observed via kprobe on kfd_ioctl_ais.
// Tracks per-GPU (via gpu_id from the handle upper-32 bits) and per-PCIe device
// (via pcie_id = pdev->devfn encoded as domain:bus:dev.fn string from
// bpftrace).
struct AisRecord {
    uint64_t seq = 0; // monotonic per-monitor sequence number
    int pid = 0;      // calling process pid
    std::string comm; // process name
    AisOp op = AisOp::Unknown;
    uint32_t gpu_id = 0;      // KFD gpu_id (upper 32 bits of handle)
    std::string pcie_id;      // PCIe BDF string e.g. "0000:01:00.0"
    PcieDeviceInfo pcie_info; // device type, vendor, raw IDs
    uint64_t size_req = 0;    // requested transfer size (in->size)
    uint64_t size_copied = 0; // actual bytes transferred (out->size_copied)
    int error = 0;            // 0 = success, negative errno on failure
    double submit_ts = 0;     // CLOCK_MONOTONIC_RAW seconds at ioctl entry
    double complete_ts = 0;   // CLOCK_MONOTONIC_RAW seconds at ioctl return
    bool completed = false;

    double latency_sec() const {
        return completed ? (complete_ts - submit_ts) : 0.0;
    }
    bool is_error() const { return error != 0; }
};

} // namespace hsasnoop
