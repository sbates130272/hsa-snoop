#include "sdma.h"

#include "model.h"

#include <dirent.h>

#include <fstream>
#include <string>

namespace sdma {

const char* VersionName(Version version) {
    switch (version) {
    case Version::V4:
        return "v4";
    case Version::V5:
        return "v5";
    case Version::V6:
        return "v6";
    case Version::V7:
        return "v7";
    case Version::Unknown:
        return "unknown";
    }
    return "unknown";
}

bool IsSupportedVersion(Version version) {
    return version == Version::V4 || version == Version::V6;
}

Version VersionFromGfxTarget(uint32_t gfx_target_version) {
    switch (gfx_target_version / 10000) {
    case 9:
        return Version::V4;
    case 10:
        return Version::V5;
    case 11:
        return Version::V6;
    case 12:
        return Version::V7;
    default:
        return Version::Unknown;
    }
}

Version DetectVersion(uint32_t gpu_id, const char* topology_root) {
    DIR* dir = opendir(topology_root);
    if (!dir)
        return Version::Unknown;

    Version result = Version::Unknown;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_name[0] == '.')
            continue;

        const std::string node = std::string(topology_root) + "/" + ent->d_name;
        std::ifstream id_file(node + "/gpu_id");
        uint32_t node_gpu_id = 0;
        if (!(id_file >> node_gpu_id) || node_gpu_id != gpu_id)
            continue;

        std::ifstream properties(node + "/properties");
        std::string key;
        uint64_t value = 0;
        while (properties >> key >> value) {
            if (key == "gfx_target_version") {
                result = VersionFromGfxTarget(static_cast<uint32_t>(value));
                break;
            }
        }
        break;
    }
    closedir(dir);
    return result;
}

bool IsLinearCopy(Version version, uint8_t sub_op) {
    return (version == Version::V4 && sub_op == SUBOP_COPY_LINEAR) ||
           (version == Version::V6 &&
            (sub_op == SUBOP_COPY_LINEAR || sub_op == SUBOP_COPY_LINEAR_BC));
}

uint64_t LinearCopyBytes(Version version, uint8_t sub_op, uint32_t count_dw) {
    if (!IsLinearCopy(version, sub_op))
        return 0;
    const uint32_t mask = version == Version::V6 && sub_op == SUBOP_COPY_LINEAR
                              ? kLinearCopyCountMask30
                              : kLinearCopyCountMask22;
    return static_cast<uint64_t>(count_dw & mask) + 1;
}

const char* OpName(Version version, uint8_t op) {
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
    case OP_VERSION_SPECIFIC_16:
        if (version == Version::V4)
            return "dummy_trap";
        if (version == Version::V6)
            return "gpuvm_inv";
        return "unknown";
    case OP_GCR_REQ:
        return version == Version::V6 ? "gcr_req" : "unknown";
    case OP_V6_DUMMY_TRAP:
        return version == Version::V6 ? "dummy_trap" : "unknown";
    }
    return "unknown";
}

const char* CopySubOpName(Version version, uint8_t sub_op) {
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
    case SUBOP_COPY_LINEAR_BC:
        return version == Version::V6 ? "linear_bc" : "unknown";
    case SUBOP_COPY_TILED_BC:
        return version == Version::V6 ? "tiled_bc" : "unknown";
    case SUBOP_COPY_LINEAR_SUB_WIND_BC:
        return version == Version::V6 ? "linear_subwindow_bc" : "unknown";
    case SUBOP_COPY_TILED_SUB_WIND_BC:
        return version == Version::V6 ? "tiled_subwindow_bc" : "unknown";
    case SUBOP_COPY_T2T_SUB_WIND_BC:
        return version == Version::V6 ? "t2t_subwindow_bc" : "unknown";
    case SUBOP_COPY_LINEAR_SUB_WIND_LARGE:
        return version == Version::V6 ? "linear_subwindow_large" : "unknown";
    }
    return "unknown";
}

namespace {

constexpr uint32_t kWriteCountMask20 = 0x000FFFFF;

uint32_t BroadcastCopyPacketLen(uint8_t sub) {
    if (sub == SUBOP_COPY_LINEAR)
        return 9;
    if (sub == SUBOP_COPY_TILED)
        return 16;
    return 0;
}

uint32_t CopyPacketLenV4(uint32_t header) {
    const uint8_t sub = HeaderSubOp(header);
    if (HeaderBroadcast(header))
        return BroadcastCopyPacketLen(sub);
    switch (sub) {
    case SUBOP_COPY_LINEAR:
    case SUBOP_COPY_DIRTY_PAGE:
    case SUBOP_COPY_LINEAR_PHY:
        return 7;
    case SUBOP_COPY_SOA:
        return 8;
    case SUBOP_COPY_LINEAR_SUB_WIND:
    case SUBOP_COPY_TILED:
        return 13;
    case SUBOP_COPY_TILED_SUB_WIND:
        return 14;
    case SUBOP_COPY_T2T_SUB_WIND:
        return 15;
    default:
        return 0;
    }
}

uint32_t CopyPacketLenV6(uint32_t header) {
    const uint8_t sub = HeaderSubOp(header);
    if (HeaderBroadcast(header))
        return BroadcastCopyPacketLen(sub);
    switch (sub) {
    case SUBOP_COPY_LINEAR:
    case SUBOP_COPY_DIRTY_PAGE:
    case SUBOP_COPY_LINEAR_PHY:
    case SUBOP_COPY_LINEAR_BC:
        return 7;
    case SUBOP_COPY_SOA:
        return 8;
    case SUBOP_COPY_LINEAR_SUB_WIND:
    case SUBOP_COPY_TILED:
    case SUBOP_COPY_TILED_BC:
    case SUBOP_COPY_LINEAR_SUB_WIND_BC:
        return 13;
    case SUBOP_COPY_TILED_SUB_WIND_BC:
        return 14;
    case SUBOP_COPY_T2T_SUB_WIND_BC:
        return 15;
    case SUBOP_COPY_TILED_SUB_WIND:
        return 17;
    case SUBOP_COPY_T2T_SUB_WIND:
        return 18;
    case SUBOP_COPY_LINEAR_SUB_WIND_LARGE:
        return 20;
    default:
        return 0;
    }
}

uint32_t PollPacketLen(Version version, uint8_t sub) {
    switch (sub) {
    case 0: // POLL_REGMEM
        return 6;
    case 1: // POLL_REG_WRITE_MEM
        return 4;
    case 2: // POLL_DBIT_WRITE_MEM
        return 5;
    case 3: // POLL_MEM_VERIFY
        return 13;
    case 4: // VM_INVALIDATION (v6)
        return version == Version::V6 ? 4 : 0;
    default:
        return 0;
    }
}

uint32_t PtePdePacketLen(Version version, uint8_t sub) {
    switch (sub) {
    case 1: // COPY
        return 8;
    case 2: // RMW
        return version == Version::V4 ? 7 : 8;
    case 3: // COPY_BACKWARDS
        return 7;
    default:
        return 0;
    }
}

} // namespace

uint32_t PacketLenDwords(Version version, const uint32_t* dw, uint32_t navail) {
    if (!IsSupportedVersion(version))
        return 0;
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
        return version == Version::V4 ? CopyPacketLenV4(dw0)
                                      : CopyPacketLenV6(dw0);
    case OP_WRITE: {
        // header, dst_addr_lo, dst_addr_hi, count(dwords-1), data[count+1].
        if (sub != 0 || navail < 4)
            return 0; // need the count dword to know the length
        return 5 + (dw[3] & kWriteCountMask20);
    }
    case OP_FENCE:
        return 4;
    case OP_TRAP:
        return 2;
    case OP_SEM:
        return 3;
    case OP_POLL_REGMEM:
        return PollPacketLen(version, sub);
    case OP_COND_EXE:
        return 5;
    case OP_ATOMIC:
        return 8;
    case OP_CONST_FILL:
        return sub == 0 ? 5 : (sub == 1 ? 6 : 0);
    case OP_PTEPDE:
        return PtePdePacketLen(version, sub);
    case OP_TIMESTAMP:
        return sub <= 2 ? 3 : 0;
    case OP_SRBM_WRITE:
        return 3;
    case OP_PRE_EXE:
        return 2;
    case OP_INDIRECT:
        return 6;
    case OP_VERSION_SPECIFIC_16:
        return version == Version::V4 ? 2 : 4;
    case OP_GCR_REQ:
        return version == Version::V6 ? 5 : 0;
    case OP_V6_DUMMY_TRAP:
        return version == Version::V6 ? 2 : 0;
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
