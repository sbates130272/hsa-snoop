#include "sdma.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <string>

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "sdma packet test failed: %s\n", message);
    return condition;
}

uint32_t Header(uint8_t op, uint8_t sub = 0) {
    return static_cast<uint32_t>(op) | (static_cast<uint32_t>(sub) << 8);
}

bool TestVersionMapping() {
    bool ok = true;
    ok &= Expect(sdma::VersionFromGfxTarget(90010) == sdma::Version::V4,
                 "gfx90a must map to SDMA v4");
    ok &= Expect(sdma::VersionFromGfxTarget(90402) == sdma::Version::V4,
                 "gfx942 must map to SDMA v4");
    ok &= Expect(sdma::VersionFromGfxTarget(100300) == sdma::Version::V5,
                 "gfx1030 must map to unsupported SDMA v5");
    ok &= Expect(sdma::VersionFromGfxTarget(110001) == sdma::Version::V6,
                 "gfx1101 must map to SDMA v6");
    ok &= Expect(sdma::VersionFromGfxTarget(120001) == sdma::Version::V7,
                 "gfx1201 must map to unsupported SDMA v7");
    ok &= Expect(sdma::VersionFromGfxTarget(0) == sdma::Version::Unknown,
                 "missing gfx target must remain unknown");
    return ok;
}

bool TestTopologyDetection() {
    char root_template[] = "/tmp/hsa-snoop-sdma-topology-XXXXXX";
    char* root = mkdtemp(root_template);
    if (!root)
        return Expect(false, "mkdtemp failed");

    const std::string node = std::string(root) + "/1";
    bool ok = Expect(mkdir(node.c_str(), 0700) == 0,
                     "fake topology node creation failed");
    if (ok) {
        std::ofstream(node + "/gpu_id") << "43288\n";
        std::ofstream(node + "/properties") << "simd_count 120\n"
                                            << "gfx_target_version 110001\n"
                                            << "sdma_fw_version 20\n";
        ok &= Expect(sdma::DetectVersion(43288, root) == sdma::Version::V6,
                     "topology lookup did not detect SDMA v6");
        ok &= Expect(sdma::DetectVersion(1, root) == sdma::Version::Unknown,
                     "unknown gpu_id must not inherit another GPU version");
    }

    unlink((node + "/properties").c_str());
    unlink((node + "/gpu_id").c_str());
    rmdir(node.c_str());
    rmdir(root);
    return ok;
}

bool TestLinearCopyLayouts() {
    bool ok = true;
    constexpr uint32_t count = 0x04000003;
    ok &= Expect(sdma::LinearCopyBytes(sdma::Version::V4,
                                       sdma::SUBOP_COPY_LINEAR, count) == 4,
                 "v4 COPY_LINEAR must use the 22-bit COUNT field");
    ok &=
        Expect(sdma::LinearCopyBytes(sdma::Version::V6, sdma::SUBOP_COPY_LINEAR,
                                     count) == 0x04000004ULL,
               "v6 COPY_LINEAR must use the 30-bit COUNT field");
    ok &= Expect(sdma::LinearCopyBytes(sdma::Version::V6,
                                       sdma::SUBOP_COPY_LINEAR_BC, count) == 4,
                 "v6 COPY_LINEAR_BC must use the 22-bit COUNT field");

    uint32_t packet[4] = {};
    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_LINEAR);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 7,
                 "v4 COPY_LINEAR length must be 7 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 7,
                 "v6 COPY_LINEAR length must be 7 dwords");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_LINEAR) | (1u << 27);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 9,
                 "v4 broadcast linear length must be 9 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 9,
                 "v6 broadcast linear length must be 9 dwords");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_TILED) | (1u << 27);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 16,
                 "v4 L2T broadcast length must be 16 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 16,
                 "v6 L2T broadcast length must be 16 dwords");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_TILED_SUB_WIND);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 14,
                 "v4 tiled subwindow length must be 14 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 17,
                 "v6 tiled subwindow length must be 17 dwords");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_LINEAR_SUB_WIND_LARGE);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 0,
                 "v6-only copy layout must be rejected by v4");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 20,
                 "v6 large linear subwindow length must be 20 dwords");
    return ok;
}

bool TestControlPacketLayouts() {
    bool ok = true;
    uint32_t packet[4] = {};

    packet[0] = Header(sdma::OP_COND_EXE);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 5,
                 "v4 COND_EXE length must be 5 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 5,
                 "v6 COND_EXE length must be 5 dwords");

    packet[0] = Header(sdma::OP_ATOMIC);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 8,
                 "v4 ATOMIC length must be 8 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 8,
                 "v6 ATOMIC length must be 8 dwords");

    packet[0] = Header(sdma::OP_VERSION_SPECIFIC_16);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 2,
                 "opcode 16 must be v4 DUMMY_TRAP");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 4,
                 "opcode 16 must be v6 GPUVM_INV");

    packet[0] = Header(sdma::OP_V6_DUMMY_TRAP);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 0,
                 "opcode 32 must be rejected by v4");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 2,
                 "opcode 32 must be v6 DUMMY_TRAP");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::Unknown, packet, 1) == 0,
                 "unknown SDMA versions must fail closed");
    return ok;
}

bool TestPacketNames() {
    bool ok = true;
    ok &= Expect(
        std::string(sdma::CopySubOpName(
            sdma::Version::V6, sdma::SUBOP_COPY_LINEAR_SUB_WIND_LARGE)) ==
            "linear_subwindow_large",
        "v6 copy sub-op name must use the v6 packet definition");
    ok &= Expect(std::string(sdma::CopySubOpName(
                     sdma::Version::V4,
                     sdma::SUBOP_COPY_LINEAR_SUB_WIND_LARGE)) == "unknown",
                 "v6-only copy sub-op must be unknown on v4");
    ok &= Expect(std::string(sdma::CopySubOpName(sdma::Version::V6, 0xff)) ==
                     "unknown",
                 "unknown copy sub-op must use the standard unknown name");
    ok &= Expect(std::string(sdma::CopySubOpName(sdma::Version::V6,
                                                 sdma::SUBOP_COPY_LINEAR_BC)) ==
                     "linear_bc",
                 "v6 BC copy sub-op must not be labelled as broadcast");
    return ok;
}

} // namespace

int main() {
    return TestVersionMapping() && TestTopologyDetection() &&
                   TestLinearCopyLayouts() && TestControlPacketLayouts() &&
                   TestPacketNames()
               ? 0
               : 1;
}
