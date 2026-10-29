# Examples

`gfx-test.hip` — a HIP compute workload that launches `vadd`, `vscale`, and
`vdot` kernels in a configurable batch/loop pattern. Keeps AQL packets queued
long enough to be captured cleanly by hsa-snoop. Used to validate AQL queue
capture.

`sdma-test.hip` — a HIP SDMA stress workload that drives `hipMemcpyAsync`
across multiple async streams to generate H2D, D2D, and D2H copy traffic.
Used to validate SDMA queue capture.

Both examples are built automatically alongside hsa-snoop when ROCm is
detected. GPU targets are auto-detected via `rocm_agent_enumerator`; pass
`-DGPU_TARGETS=gfxNNNN` to override.

## gfx-test

```
build/examples/gfx-test --help
sudo ./hsa-snoop -- build/examples/gfx-test
sudo ./hsa-snoop -- build/examples/gfx-test --loops 5 --batch 20 --sleep-ms 100
# open hsa-snoop.pftrace at https://ui.perfetto.dev
```

Key options:

| Flag | Default | Description |
| --- | --- | --- |
| `-n`, `--elements N` | 16777216 | Float elements per vector |
| `-i`, `--iters N` | 1024 | Inner loop count inside vdot kernel |
| `-b`, `--batch N` | 10 | Kernel launches per outer iteration |
| `-s`, `--sleep-ms N` | 200 | Sleep between outer iterations (ms) |
| `-l`, `--loops N` | 0 | Outer iterations; 0 = run forever |

## sdma-test

```
build/examples/sdma-test --help
sudo ./hsa-snoop -- build/examples/sdma-test
sudo ./hsa-snoop -- build/examples/sdma-test --streams 8 --buf-mb 512 --iters 100
# open hsa-snoop.pftrace at https://ui.perfetto.dev
```

Key options:

| Flag | Default | Description |
| --- | --- | --- |
| `-s`, `--streams N` | 4 | Number of async streams |
| `-m`, `--buf-mb N` | 256 | Per-stream buffer size (MB) |
| `-i`, `--iters N` | 40 | Copy iterations |
| `-r`, `--report N` | 10 | Print throughput every N iterations |
