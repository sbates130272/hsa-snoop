#include "sdma.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
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
                 "gfx1201 must map to SDMA v7");
    ok &= Expect(sdma::VersionFromGfxTarget(120500) == sdma::Version::V7Gfx125,
                 "gfx1250 must map to the gfx12.5+ SDMA v7 packet set");
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

    // Bit 27 is `broadcast` only in the LINEAR and TILED headers. The v4
    // TILED_SUB_WIND header uses bits [27:24] for mip_id, so a mip_id >= 8
    // must not be mistaken for a broadcast copy and rejected.
    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_TILED_SUB_WIND) |
                (0xAu << 24); // mip_id = 10, sets bit 27
    ok &= Expect(!sdma::HeaderBroadcast(packet[0]),
                 "mip_id bit 27 must not read as broadcast");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 14,
                 "v4 tiled subwindow with mip_id >= 8 must still be 14 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 17,
                 "v6 tiled subwindow with bit 27 set must still be 17 dwords");

    packet[0] =
        Header(sdma::OP_COPY, sdma::SUBOP_COPY_T2T_SUB_WIND) | (1u << 27);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 15,
                 "v4 t2t subwindow must ignore bit 27");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 18,
                 "v6 t2t subwindow must ignore bit 27");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_LINEAR_SUB_WIND_LARGE);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 0,
                 "v6-only copy layout must be rejected by v4");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 1) == 20,
                 "v6 large linear subwindow length must be 20 dwords");
    return ok;
}

// What ROCr writes to a user-mode queue on gfx12, by GPU generation.
bool TestGfx12Layouts() {
    bool ok = true;
    const sdma::Version v7 = sdma::Version::V7;
    const sdma::Version g125 = sdma::Version::V7Gfx125;
    uint32_t packet[16] = {};

    // Below gfx12.5 a linear copy is the v6 seven-dword packet.
    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_LINEAR);
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 7,
                 "v7 linear copy must be 7 dwords");
    ok &= Expect(sdma::CopyBodyOffset(v7, packet[0]) == 1,
                 "v7 copy block must follow the header");

    // gfx12.5+: the wait and signal header bits add 7 and 5 dwords.
    const uint32_t kWait = 1u << 30, kSignal = 1u << 31;
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 7,
                 "gfx12.5 plain linear copy must be 7 dwords");
    packet[0] = Header(sdma::OP_COPY) | kWait;
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 14,
                 "gfx12.5 copy with wait must be 14 dwords");
    ok &= Expect(sdma::CopyBodyOffset(g125, packet[0]) == 8,
                 "gfx12.5 copy block must follow the wait block");
    packet[0] = Header(sdma::OP_COPY) | kSignal;
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 12,
                 "gfx12.5 copy with signal must be 12 dwords");
    ok &= Expect(sdma::CopyBodyOffset(g125, packet[0]) == 1,
                 "gfx12.5 copy block must follow the header without wait");
    packet[0] = Header(sdma::OP_COPY) | kWait | kSignal;
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 19,
                 "gfx12.5 copy with wait and signal must be 19 dwords");
    // The same bits mean something else on older generations.
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 7,
                 "wait/signal bits must not change a v7 copy length");

    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_SWAP) | kSignal;
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 12,
                 "gfx12.5 swap copy with signal must be 12 dwords");
    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_SWAP);
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 7,
                 "v7 swap copy must be 7 dwords");
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 1) == 7,
                 "v4 swap copy must be 7 dwords");

    // Multicast: one address pair per destination, count minus one in the
    // parameters dword, which follows the count dword.
    packet[0] = Header(sdma::OP_COPY, sdma::SUBOP_COPY_MULTICAST);
    packet[2] = 2; // three destinations
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 16) == 1 + 4 + 6,
                 "gfx12.5 multicast copy to 3 destinations must be 11 dwords");
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 2) ==
                     sdma::kLenNeedMoreDwords,
                 "multicast length must wait for the parameters dword");
    packet[0] |= kWait | kSignal;
    packet[2] = 0;
    packet[9] = 1; // two destinations, parameters after the wait block
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 16) == 1 + 7 + 4 + 4 + 5,
                 "gfx12.5 multicast with wait and signal has the wrong length");
    packet[2] = 0;
    packet[9] = 0;

    // 64-bit fence and poll, and the wider cache-rinse request.
    packet[0] = Header(sdma::OP_FENCE, 2);
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 5,
                 "gfx12.5 64-bit fence must be 5 dwords");
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 4,
                 "v7 fence must be 4 dwords");
    packet[0] = Header(sdma::OP_FENCE);
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 4,
                 "gfx12.5 32-bit fence must be 4 dwords");
    packet[0] = Header(sdma::OP_POLL_REGMEM, 5);
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 8,
                 "gfx12.5 64-bit poll must be 8 dwords");
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 0,
                 "64-bit poll must be unknown below gfx12.5");
    packet[0] = Header(sdma::OP_GCR_REQ, 1);
    ok &= Expect(sdma::PacketLenDwords(g125, packet, 1) == 6,
                 "gfx12.5 cache-rinse request must be 6 dwords");
    ok &= Expect(sdma::PacketLenDwords(v7, packet, 1) == 5,
                 "v7 cache-rinse request must be 5 dwords");
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

bool TestWritePacketLayouts() {
    bool ok = true;
    uint32_t packet[4] = {};
    packet[0] = Header(sdma::OP_WRITE);

    packet[3] = 0;
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V4, packet, 4) == 5,
                 "WRITE with one data dword must be 5 dwords");

    packet[3] = 3 | (2u << 24) | (5u << 26);
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 4) == 8,
                 "WRITE length must ignore SW and cache-policy fields");

    packet[3] = 0x000FFFFF;
    ok &= Expect(sdma::PacketLenDwords(sdma::Version::V6, packet, 4) ==
                     0x00100004,
                 "WRITE must accept the maximum 20-bit COUNT");
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
                   TestLinearCopyLayouts() && TestGfx12Layouts() &&
                   TestControlPacketLayouts() && TestWritePacketLayouts() &&
                   TestPacketNames()
               ? 0
               : 1;
}
