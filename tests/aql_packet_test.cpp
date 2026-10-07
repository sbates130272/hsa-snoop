#include "aql.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

bool Expect(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "aql packet test failed: %s\n", message);
    return condition;
}

bool TestPacketSizes() {
    bool ok = true;
    ok &= Expect(sizeof(aql::KernelDispatchPacket) == 64,
                 "KernelDispatchPacket must be 64 bytes");
    ok &= Expect(sizeof(aql::BarrierPacket) == 64,
                 "BarrierPacket must be 64 bytes");
    ok &= Expect(sizeof(aql::AgentDispatchPacket) == 64,
                 "AgentDispatchPacket must be 64 bytes");
    ok &= Expect(aql::kPacketSize == 64, "kPacketSize must be 64");
    return ok;
}

bool TestHeaderType() {
    bool ok = true;
    // Each type occupies bits [7:0] of the 16-bit header.
    ok &= Expect(aql::HeaderType(static_cast<uint8_t>(
                     aql::PacketType::Invalid)) == aql::PacketType::Invalid,
                 "Invalid type must round-trip through header");
    ok &= Expect(aql::HeaderType(
                     static_cast<uint8_t>(aql::PacketType::KernelDispatch)) ==
                     aql::PacketType::KernelDispatch,
                 "KernelDispatch type must round-trip through header");
    ok &= Expect(
        aql::HeaderType(static_cast<uint8_t>(aql::PacketType::BarrierAnd)) ==
            aql::PacketType::BarrierAnd,
        "BarrierAnd type must round-trip through header");
    ok &= Expect(aql::HeaderType(static_cast<uint8_t>(
                     aql::PacketType::BarrierOr)) == aql::PacketType::BarrierOr,
                 "BarrierOr type must round-trip through header");
    ok &= Expect(
        aql::HeaderType(static_cast<uint8_t>(aql::PacketType::AgentDispatch)) ==
            aql::PacketType::AgentDispatch,
        "AgentDispatch type must round-trip through header");
    // High byte of header must not bleed into the type field.
    uint16_t dispatch_with_barrier =
        static_cast<uint8_t>(aql::PacketType::KernelDispatch) |
        (1u << aql::kHeaderBarrierShift);
    ok &= Expect(aql::HeaderType(dispatch_with_barrier) ==
                     aql::PacketType::KernelDispatch,
                 "barrier bit must not corrupt type field");
    return ok;
}

bool TestHeaderBarrier() {
    bool ok = true;
    uint16_t no_barrier = static_cast<uint8_t>(aql::PacketType::KernelDispatch);
    uint16_t with_barrier = no_barrier | (1u << aql::kHeaderBarrierShift);
    ok &= Expect(!aql::HeaderBarrier(no_barrier),
                 "barrier bit absent when not set");
    ok &= Expect(aql::HeaderBarrier(with_barrier),
                 "barrier bit present when set");
    return ok;
}

bool TestPacketTypeNames() {
    bool ok = true;
    ok &= Expect(std::string(aql::PacketTypeName(aql::PacketType::Invalid)) ==
                     "invalid",
                 "Invalid name must be 'invalid'");
    ok &= Expect(std::string(aql::PacketTypeName(
                     aql::PacketType::KernelDispatch)) == "kernel_dispatch",
                 "KernelDispatch name must be 'kernel_dispatch'");
    ok &=
        Expect(std::string(aql::PacketTypeName(aql::PacketType::BarrierAnd)) ==
                   "barrier_and",
               "BarrierAnd name must be 'barrier_and'");
    ok &= Expect(std::string(aql::PacketTypeName(aql::PacketType::BarrierOr)) ==
                     "barrier_or",
                 "BarrierOr name must be 'barrier_or'");
    ok &= Expect(std::string(aql::PacketTypeName(
                     aql::PacketType::AgentDispatch)) == "agent_dispatch",
                 "AgentDispatch name must be 'agent_dispatch'");
    ok &= Expect(std::string(aql::PacketTypeName(
                     aql::PacketType::VendorSpecific)) == "vendor_specific",
                 "VendorSpecific name must be 'vendor_specific'");
    return ok;
}

bool TestGridDims() {
    bool ok = true;
    // setup bits [1:0] encode the grid dimensionality.
    ok &= Expect(aql::GridDims(0x0) == 0, "0 dims must encode as 0");
    ok &= Expect(aql::GridDims(0x1) == 1, "1D grid must encode as 1");
    ok &= Expect(aql::GridDims(0x2) == 2, "2D grid must encode as 2");
    ok &= Expect(aql::GridDims(0x3) == 3, "3D grid must encode as 3");
    // Higher bits must not contaminate the dim field.
    ok &=
        Expect(aql::GridDims(0xFC) == 0, "high bits must not bleed into dims");
    ok &= Expect(aql::GridDims(0xFF) == 3, "all-ones setup must still give 3");
    return ok;
}

bool TestKernelDispatchLayout() {
    bool ok = true;
    aql::KernelDispatchPacket p;
    std::memset(&p, 0, sizeof(p));
    // Verify field offsets match the AQL spec (hsa.h).
    ok &= Expect(offsetof(aql::KernelDispatchPacket, header) == 0,
                 "header at offset 0");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, setup) == 2,
                 "setup at offset 2");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, workgroup_size_x) == 4,
                 "workgroup_size_x at offset 4");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, grid_size_x) == 12,
                 "grid_size_x at offset 12");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, kernel_object) == 32,
                 "kernel_object at offset 32");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, kernarg_address) == 40,
                 "kernarg_address at offset 40");
    ok &= Expect(offsetof(aql::KernelDispatchPacket, completion_signal) == 56,
                 "completion_signal at offset 56");
    return ok;
}

bool TestBarrierLayout() {
    bool ok = true;
    ok &= Expect(offsetof(aql::BarrierPacket, dep_signal) == 8,
                 "dep_signal array at offset 8");
    ok &= Expect(offsetof(aql::BarrierPacket, completion_signal) == 56,
                 "completion_signal at offset 56");
    return ok;
}

} // namespace

int main() {
    return TestPacketSizes() && TestHeaderType() && TestHeaderBarrier() &&
                   TestPacketTypeNames() && TestGridDims() &&
                   TestKernelDispatchLayout() && TestBarrierLayout()
               ? 0
               : 1;
}
