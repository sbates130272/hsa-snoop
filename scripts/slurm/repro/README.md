# Reproducers

Standalone programs used to root-cause the `--mem-snoop` findings described in the
commit messages on this branch. Neither is part of the build; compile them by
hand on an allocated node.

## `probe-cross-process-read.c`

Walks a live process's `/proc/<pid>/maps` and, for every readable region of at
least 3 MiB, attempts the same read two ways: `process_vm_readv` (what
[`ReadProcMem`](../../../src/proc_mem.cpp) uses)
and `pread` on `/proc/<pid>/mem` (what `ReadU64ViaMem` uses). Prints a table of
which mappings each method can read.

This is what showed that on MI300X the HIP device buffers are `rw-s` mappings
of the DRM render node, that `process_vm_readv` returns EFAULT on exactly those
mappings, and that `pread` succeeds on all of them.

```
gcc -O1 -o probe-cross-process-read probe-cross-process-read.c
./gfx-test -n 1048576 -b 4 -l 40 -s 200 &
./probe-cross-process-read $!
```

## `scratch-test.hip`

A deliberately scratch-heavy kernel: a per-thread `float big[256]` with dynamic
indexing so it cannot be kept in registers, forcing a large private segment.
`gfx-test`'s kernels all report `scratch=0B`, which makes it impossible to tell
whether the reported scratch figure is per-work-item or per-grid.

Used to establish that trace mode reports 1040 B per work-item while the
Prometheus exporter reports `private_seg_bytes * grid_size` for the same
quantity.

```
hipcc --offload-arch=gfx90a -O2 -o scratch-test scratch-test.hip
hsa-snoop --mem-snoop -- ./scratch-test 4
```
