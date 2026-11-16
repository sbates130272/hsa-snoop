# Kernel Timing

Per-kernel timing for the `gfx-test` HIP kernels (`vadd`, `vscale`,
`vdot`), captured by every hardware-test run on `main` against an
emulated gfx1250 GPU. Helper kernels from the HIP runtime appear too.

Two durations are recorded for each dispatch, and they are not the same
measurement:

GPU ticks

:   The interval between the start and end timestamps the GPU command
    processor writes into the dispatch's completion signal. Present only
    when the workload runs with profiling enabled, described below.
    These are raw GPU clock ticks, not a time. The start is when the
    command processor accepted the dispatch, so kernels launched back to
    back overlap and the later ones include time spent waiting.

Queue µs

:   The time from when `hsa-snoop` first saw the packet on the ring to
    when it saw the dispatch finish, by polling. Always present, and no
    finer than the poll interval. A dispatch with a completion signal
    finishes when the signal fires; one without is counted as finished
    when the command processor takes it off the queue, which is earlier.

The GPU here is emulated, so neither number says how fast real hardware
runs these kernels. What the page tracks is whether they move from one
commit to the next.

<p id="timing-latest" class="perf-empty">Loading the published history…</p>

## Latest Run

<div id="timing-table" markdown="0"></div>

## Trends

Median per kernel, one point per run.

<div id="timing-charts" class="perf-charts" markdown="0"></div>

## Enabling GPU Timestamps

Two things have to be true for a dispatch to carry timestamps.

Profiling must be enabled on the queue. `libhsa-snoop-profiler.so` does
this: it is an `HSA_TOOLS_LIB` plugin that the HSA runtime loads at
start-up, and it enables profiling on every compute queue the workload
creates.

The dispatch must have a completion signal, because that is where the
command processor writes the timestamps. HIP only attaches one to a
kernel launch when command profiling is on, so set
`GPU_FORCE_QUEUE_PROFILING=1` as well.

```
sudo env HSA_TOOLS_LIB=$PWD/build/libhsa-snoop-profiler.so \
  GPU_FORCE_QUEUE_PROFILING=1 \
  ./build/hsa-snoop --log-dispatches dispatches.ndjson -- ./my_hip_app
```

`hsa-snoop` reads the timestamps out of the signal when it fires, and
`--log-dispatches` writes them as `gpu_start_ticks` and `gpu_end_ticks`.

## Where The Numbers Come From

The `vm-hardware-test` workflow runs
`.github/ci/report/capture-kernel-timing.sh` in the test VM after the
hardware tests pass, and fails the run if the shim does not load, no
dispatch resolves to a kernel name, or none carries GPU timestamps.
`.github/ci/report/publish-kernel-timing.py` then
summarises the dispatch log per kernel, appends one record to
`timing/history.jsonl` on the `gh-pages` branch and refreshes the badge.
Pull requests get the same table in the workflow run summary, without
publishing.
