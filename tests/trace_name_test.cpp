// Unit test for the per-queue trace file name used in --all mode.

#include "trace_writer.h"

#include <cstdio>
#include <string>

using hsasnoop::TraceFileStem;

int main() {
    struct Case {
        const char* comm;
        uint64_t uid;
        const char* want;
    };
    const Case cases[] = {
        {"gfx-test", 1, "gfx-test-1"},
        {"python3.12", 42, "python3.12-42"},
        // comm is set by the process: no path separators, spaces or control
        // characters may reach the file name.
        {"a/b", 2, "a_b-2"},
        {"../../etc", 3, "_._.._etc-3"},
        {"my app\t", 4, "my_app_-4"},
        {".hidden", 5, "_hidden-5"},
        {"..", 6, "_.-6"},
        {"caf\xc3\xa9", 7, "caf__-7"},
        {"", 8, "queue-8"},
    };
    int failures = 0;
    for (const Case& c : cases) {
        const std::string got = TraceFileStem(c.comm, c.uid);
        if (got != c.want) {
            std::fprintf(stderr,
                         "FAIL: TraceFileStem(\"%s\", %lu) = \"%s\", "
                         "want \"%s\"\n",
                         c.comm, static_cast<unsigned long>(c.uid), got.c_str(),
                         c.want);
            ++failures;
        }
    }
    if (!failures)
        std::fprintf(stderr, "PASS trace-name-test\n");
    return failures ? 1 : 0;
}
