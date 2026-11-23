// child_mem.h - for tests that stand a forked child in for another process.
#pragma once
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>

#include "proc_mem.h"

// Whether this process may read the memory of a child of its own. Not so for
// a normal user where ptrace is restricted to privileged processes
// (kernel.yama.ptrace_scope 2 or 3); tests that need it must then be skipped,
// not failed. Prints the SKIP line itself.
inline bool ChildMemReadable(const char* test) {
    static int readable = -1;
    if (readable < 0) {
        static uint64_t probe = 0x600dfeedULL;
        const pid_t c = fork();
        if (c == 0)
            for (;;)
                pause();
        uint64_t got = 0;
        const uint64_t va = reinterpret_cast<uint64_t>(&probe);
        readable = c > 0 && (hsasnoop::ReadU64(c, va, &got) ||
                             hsasnoop::ReadU64ViaMem(c, va, &got));
        if (c > 0) {
            kill(c, SIGKILL);
            waitpid(c, nullptr, 0);
        }
    }
    if (!readable)
        std::fprintf(stderr,
                     "SKIP: %s: cannot read a child's memory (is "
                     "kernel.yama.ptrace_scope 2 or 3?)\n",
                     test);
    return readable;
}
