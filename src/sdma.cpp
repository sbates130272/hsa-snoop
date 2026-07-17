#include "sdma.h"

#include "model.h"

namespace sdma {

const char* OpName(uint8_t op) {
    switch (op) {
    case OP_NOP:
        return "nop";
    case OP_COPY:
        return "copy";
    case OP_WRITE:
        return "write";
    case OP_INDIRECT:
        return "indirect";
    case OP_FENCE:
        return "fence";
    case OP_TRAP:
        return "trap";
    case OP_SEM:
        return "semaphore";
    case OP_POLL_REGMEM:
        return "poll_regmem";
    case OP_COND_EXE:
        return "cond_exe";
    case OP_ATOMIC:
        return "atomic";
    case OP_CONST_FILL:
        return "const_fill";
    case OP_PTEPDE:
        return "ptepde";
    case OP_TIMESTAMP:
        return "timestamp";
    case OP_SRBM_WRITE:
        return "srbm_write";
    case OP_PRE_EXE:
        return "pre_exe";
    case OP_GCR_REQ:
        return "gcr_req";
    case OP_DUMMY_TRAP:
        return "dummy_trap";
    }
    return "unknown";
}

const char* CopySubOpName(uint8_t sub_op) {
    switch (sub_op) {
    case SUBOP_COPY_LINEAR:
        return "linear";
    case SUBOP_COPY_TILED:
        return "tiled";
    case SUBOP_COPY_SOA:
        return "soa";
    case SUBOP_COPY_LINEAR_SUB_WIND:
        return "linear_subwindow";
    case SUBOP_COPY_TILED_SUB_WIND:
        return "tiled_subwindow";
    case SUBOP_COPY_T2T_SUB_WIND:
        return "t2t_subwindow";
    case SUBOP_COPY_DIRTY_PAGE:
        return "dirty_page";
    case SUBOP_COPY_LINEAR_PHY:
        return "linear_physical";
    }
    return "sub?";
}

uint32_t PacketLenDwords(const uint32_t* dw, uint32_t navail) {
    if (navail < 1)
        return 0;
    const uint32_t dw0 = dw[0];
    const uint8_t op = HeaderOp(dw0);
    const uint8_t sub = HeaderSubOp(dw0);

    switch (op) {
    case OP_NOP:
        // Header + `count` trailing dwords (count may be 0).
        return 1 + NopCount(dw0);
    case OP_COPY:
        switch (sub) {
        case SUBOP_COPY_LINEAR:
            return 7;
        case SUBOP_COPY_LINEAR_PHY:
        case SUBOP_COPY_DIRTY_PAGE:
        case SUBOP_COPY_TILED:
            return 12;
        case SUBOP_COPY_LINEAR_SUB_WIND:
            return 13;
        case SUBOP_COPY_TILED_SUB_WIND:
            return 14;
        case SUBOP_COPY_T2T_SUB_WIND:
            return 15;
        default:
            return 0; // unknown copy layout -> resync
        }
    case OP_WRITE: {
        // header, dst_addr_lo, dst_addr_hi, count(dwords-1), data[count+1].
        if (navail < 4)
            return 0; // need the count dword to know the length
        return 4 + (dw[3] + 1);
    }
    case OP_FENCE:
        return 4;
    case OP_TRAP:
        return 2;
    case OP_SEM:
        return 3;
    case OP_POLL_REGMEM:
        return 6;
    case OP_COND_EXE:
        return 4;
    case OP_ATOMIC:
        return 9;
    case OP_CONST_FILL:
        return 5;
    case OP_TIMESTAMP:
        return 3;
    case OP_SRBM_WRITE:
        return 3;
    case OP_PRE_EXE:
        return 2;
    case OP_GCR_REQ:
        return 5;
    case OP_INDIRECT:
        return 6;
    case OP_DUMMY_TRAP:
        return 2;
    }
    return 0; // unknown opcode
}

} // namespace sdma

namespace hsasnoop {

const char* CopyDirName(CopyDir d) {
    switch (d) {
    case CopyDir::HostToDevice:
        return "h2d";
    case CopyDir::DeviceToHost:
        return "d2h";
    case CopyDir::DeviceToDevice:
        return "d2d";
    case CopyDir::HostToHost:
        return "h2h";
    case CopyDir::Unknown:
        break;
    }
    return "unknown";
}

const char* AisOpName(AisOp op) {
    switch (op) {
    case AisOp::Read:
        return "read";
    case AisOp::Write:
        return "write";
    case AisOp::Unknown:
        break;
    }
    return "unknown";
}

} // namespace hsasnoop
