// sdma.h - Self-contained mirror of the AMD SDMA microcode packet formats.
//
// SDMA (System DMA) is the GPU's copy-engine command stream. hipMemcpyAsync,
// peer-to-peer DMA and other bulk transfers are submitted to KFD SDMA queues
// (KFD_IOC_QUEUE_TYPE_SDMA / _SDMA_XGMI), NOT the AQL compute queues -- so they
// are invisible to the AQL dispatch snoop and need their own decoder.
//
// Unlike AQL (every packet is 64 bytes), SDMA packets are variable-length,
// dword-granular microcode. Each packet's first dword is a header carrying an
// 8-bit opcode and 8-bit sub-opcode; the packet length depends on the opcode
// (and, for a few, on a count field). To walk a ring we decode the header,
// compute the length, then step forward.
//
// Layouts below follow the SDMA v4.x family used by CDNA GPUs (gfx90a / gfx942,
// MI2xx/MI3xx). They are deliberately NOT pulled from ROCm headers so hsa-snoop
// builds without -dev packages. Field positions that are hardware-revision
// sensitive (notably the linear-copy COUNT width and the ring pointer unit) are
// called out and are the first thing to confirm during on-hardware calibration
// (build+run under HSA_SNOOP_DEBUG=1 and compare the hex dumps).
#pragma once
#include <cstdint>

namespace sdma {

// SDMA opcodes (SDMA_OP_*), the low 8 bits of the header dword.
enum Op : uint8_t {
    OP_NOP = 0,
    OP_COPY = 1,
    OP_WRITE = 2,
    OP_INDIRECT = 4,
    OP_FENCE = 5,
    OP_TRAP = 6,
    OP_SEM = 7,
    OP_POLL_REGMEM = 8,
    OP_COND_EXE = 9,
    OP_ATOMIC = 10,
    OP_CONST_FILL = 11,
    OP_PTEPDE = 12,
    OP_TIMESTAMP = 13,
    OP_SRBM_WRITE = 14,
    OP_PRE_EXE = 15,
    OP_GCR_REQ = 17, // graphics-cache-rinse / invalidate request
    OP_DUMMY_TRAP = 32,
};

// COPY sub-opcodes (SDMA_SUBOP_COPY_*), header bits [15:8] when op == OP_COPY.
enum CopySubOp : uint8_t {
    SUBOP_COPY_LINEAR = 0,
    SUBOP_COPY_TILED = 1,
    SUBOP_COPY_SOA = 3,
    SUBOP_COPY_LINEAR_SUB_WIND = 4,
    SUBOP_COPY_TILED_SUB_WIND = 5,
    SUBOP_COPY_T2T_SUB_WIND = 6,
    SUBOP_COPY_DIRTY_PAGE = 7,
    SUBOP_COPY_LINEAR_PHY = 8,
};

// Header field accessors (header is the packet's first dword).
inline uint8_t HeaderOp(uint32_t dw0) { return dw0 & 0xFF; }
inline uint8_t HeaderSubOp(uint32_t dw0) { return (dw0 >> 8) & 0xFF; }
// NOP carries a count of trailing dwords to skip in bits [27:16] (14-bit).
inline uint32_t NopCount(uint32_t dw0) { return (dw0 >> 16) & 0x3FFF; }

// Linear-copy COUNT field is the byte count minus one. The width is
// hardware-revision sensitive (v4 is 22-bit / 4 MiB max; later families are
// wider). We keep the low 26 bits, which is safe for every family we target,
// and confirm the true width during calibration.
constexpr uint32_t kLinearCopyCountMask = 0x03FFFFFF;
inline uint64_t LinearCopyBytes(uint32_t count_dw) {
    return static_cast<uint64_t>(count_dw & kLinearCopyCountMask) + 1;
}

// SDMA_PKT_COPY_LINEAR (7 dwords). The common hipMemcpy path.
#pragma pack(push, 1)
struct CopyLinearPacket {
    uint32_t header;      // DW0: op=COPY, sub_op=LINEAR, misc flags [31:16]
    uint32_t count;       // DW1: byte count - 1 (masked, see LinearCopyBytes)
    uint32_t parameters;  // DW2: dst_sw/src_sw/etc.
    uint32_t src_addr_lo; // DW3
    uint32_t src_addr_hi; // DW4
    uint32_t dst_addr_lo; // DW5
    uint32_t dst_addr_hi; // DW6
};
static_assert(sizeof(CopyLinearPacket) == 28, "SDMA linear copy is 7 dwords");
#pragma pack(pop)

// Human-readable opcode / copy sub-opcode names.
const char* OpName(uint8_t op);
const char* CopySubOpName(uint8_t sub_op);

// Length in dwords of the packet at the front of `dw` (navail dwords readable).
// A few opcodes (NOP, WRITE) carry the length in a later dword, so the caller
// passes a small window. Returns 0 when the opcode is unknown, or when navail
// is too small to determine the length -- in both cases the caller stops and
// resyncs rather than mis-striding the ring.
uint32_t PacketLenDwords(const uint32_t* dw, uint32_t navail);

} // namespace sdma
