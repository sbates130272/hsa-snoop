// ais_trace_test.cpp — parsing of the trace_pipe lines the AIS entry and
// return probes produce. Needs no GPU and no root.

#include "ais_monitor.h"

#include <cstdio>
#include <string>

namespace {

const std::string kEntry = "hsasnoop_ais_e_42";
const std::string kReturn = "hsasnoop_ais_r_42";

bool Expect(bool condition, const char* message) {
    if (!condition)
        std::fprintf(stderr, "ais trace test failed: %s\n", message);
    return condition;
}

bool TestEntryLine() {
    const std::string line =
        "        ais-test-3008    [002] .....   123.456789: hsasnoop_ais_e_42: "
        "(kfd_ioctl_ais+0x0/0x2a0 [amdgpu]) gpu_id=12485 size_req=65536 op=1 "
        "fd=7";
    hsasnoop::AisTraceLine t;
    bool ok = Expect(hsasnoop::ParseAisTraceLine(line, kEntry, kReturn, &t),
                     "entry line must parse");
    ok &= Expect(t.kind == hsasnoop::AisTraceLine::Kind::Entry,
                 "entry line must be an entry");
    ok &= Expect(t.tid == 3008, "tid");
    ok &= Expect(t.comm == "ais-test", "comm");
    ok &= Expect(t.ts > 123.45 && t.ts < 123.46, "timestamp");
    ok &= Expect(t.gpu_id == 12485 && t.size_req == 65536 && t.op == 1 &&
                     t.fd == 7,
                 "entry fields");
    return ok;
}

bool TestReturnLines() {
    hsasnoop::AisTraceLine t;
    const std::string failed =
        "        ais-test-3008    [002] .....   123.456999: hsasnoop_ais_r_42: "
        "(kfd_ioctl+0x2c4/0x5d0 [amdgpu] <- kfd_ioctl_ais) ret=-19";
    bool ok = Expect(hsasnoop::ParseAisTraceLine(failed, kEntry, kReturn, &t),
                     "return line must parse");
    ok &= Expect(t.kind == hsasnoop::AisTraceLine::Kind::Return,
                 "return line must be a return");
    ok &= Expect(t.tid == 3008 && t.ret == -19, "ENODEV return value");

    const std::string good =
        "        ais-test-3008    [002] .....   123.457100: hsasnoop_ais_r_42: "
        "(kfd_ioctl+0x2c4/0x5d0 [amdgpu] <- kfd_ioctl_ais) ret=0";
    ok &= Expect(hsasnoop::ParseAisTraceLine(good, kEntry, kReturn, &t) &&
                     t.ret == 0,
                 "successful return value");

    // Without a return probe, its lines are not ours.
    ok &= Expect(!hsasnoop::ParseAisTraceLine(good, kEntry, "", &t),
                 "return line must be ignored when no return probe is set");
    return ok;
}

bool TestForeignLines() {
    hsasnoop::AisTraceLine t;
    bool ok = true;
    ok &= Expect(!hsasnoop::ParseAisTraceLine(
                     "   gfx-test-99  [000] .....  1.0: hsasnoop_cq_42: "
                     "(kfd_ioctl_create_queue+0x0/0x10) ring_base=0x1",
                     kEntry, kReturn, &t),
                 "another probe's line must not parse");
    // A different hsa-snoop instance's probe shares the prefix.
    ok &= Expect(!hsasnoop::ParseAisTraceLine(
                     "   ais-test-99  [000] .....  1.0: hsasnoop_ais_e_421: "
                     "(kfd_ioctl_ais+0x0/0x2a0) gpu_id=1 size_req=2 op=1 fd=3",
                     kEntry, kReturn, &t),
                 "a probe name that only shares a prefix must not parse");
    // A comm with brackets and dashes of its own.
    ok &= Expect(hsasnoop::ParseAisTraceLine(
                     "    my-loader[3]-4242  [001] .....  2.5: "
                     "hsasnoop_ais_e_42: (kfd_ioctl_ais+0x0/0x2a0 [amdgpu]) "
                     "gpu_id=1 size_req=2 op=2 fd=3",
                     kEntry, kReturn, &t) &&
                     t.tid == 4242 && t.comm == "my-loader[3]",
                 "a comm containing '[' and '-' must parse");
    ok &= Expect(
        !hsasnoop::ParseAisTraceLine("# tracer: nop", kEntry, kReturn, &t),
        "a header line must not parse");
    return ok;
}

} // namespace

int main() {
    return TestEntryLine() && TestReturnLines() && TestForeignLines() ? 0 : 1;
}
