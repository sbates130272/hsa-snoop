#!/usr/bin/env bash
# Run the gfx-test HIP kernels under hsa-snoop with the dispatch log on and
# leave one NDJSON line per kernel dispatch in $OUT.
#
# Runs inside the hardware-test VM, as a user who can sudo, against an
# hsa-snoop build tree. With libhsa-snoop-profiler.so present the workload
# is launched under HSA_TOOLS_LIB with HIP command profiling forced on, so
# each dispatch also carries the GPU clock ticks the command processor
# wrote into its completion signal. Without it the log still carries the
# queue-observed dur_us. TIMING_REQUIRE_SHIM=1 makes a missing shim, or a
# run with no GPU timestamps, an error.
#
# Environment: TIMING_ELEMENTS, TIMING_BATCH, TIMING_REQUIRE_SHIM,
# TIMING_DEBUG, TIMING_LOG (see below).
#
# usage: capture-kernel-timing.sh <build-dir> <out.ndjson>
set -euo pipefail

build=${1:?usage: capture-kernel-timing.sh <build-dir> <out.ndjson>}
out=${2:?usage: capture-kernel-timing.sh <build-dir> <out.ndjson>}

# Small on purpose: the lane runs against an emulated GPU, where every
# dispatch costs seconds. BATCH rounds of the three gfx-test kernels gives
# BATCH samples of each.
elements=${TIMING_ELEMENTS:-256}
batch=${TIMING_BATCH:-8}

shim=$(find "$build" -maxdepth 1 -name 'libhsa-snoop-profiler.so' | head -1)
env_args=()
if [ -n "$shim" ]; then
  echo "profiler shim: $shim"
  # The shim enables profiling on the queues, which tells the command
  # processor to timestamp dispatches. The timestamps go into a dispatch's
  # completion signal, and HIP only attaches one when command profiling is
  # on, so force that too.
  env_args=("HSA_TOOLS_LIB=$shim" "GPU_FORCE_QUEUE_PROFILING=1")
elif [ "${TIMING_REQUIRE_SHIM:-0}" = 1 ]; then
  echo "::error::libhsa-snoop-profiler.so was not built"
  exit 1
else
  echo "::warning::libhsa-snoop-profiler.so was not built; capturing queue-observed timing only"
fi

# TIMING_DEBUG=1 turns on hsa-snoop's debug log, which records what each
# completion-signal timestamp read returned. It adds stderr writes to the
# poll loop, so the timings from such a run are not comparable.
if [ "${TIMING_DEBUG:-0}" = 1 ]; then
  env_args+=("HSA_SNOOP_DEBUG=1" "HSA_SNOOP_PROFILER_DEBUG=1")
fi

log=$(mktemp)
sudo rm -f "$out"
sudo env "${env_args[@]}" "$build/hsa-snoop" \
  --format json \
  --out "${out%.ndjson}-trace.json" \
  --poll-us 1000 \
  --log-dispatches "$out" \
  -- "$build/deploy/examples/gfx-test" \
       --elements "$elements" --iters 1 --loops 1 --batch "$batch" --sleep-ms 0 \
  > "$log" 2>&1 && rc=0 || rc=$?
# TIMING_LOG=<path> keeps hsa-snoop's whole log, debug lines included. Saved
# before any check below can exit, since a failed capture is the one whose log
# is wanted.
if [ -n "${TIMING_LOG:-}" ]; then
  cp "$log" "$TIMING_LOG"
  chmod a+r "$TIMING_LOG"
fi
if [ "$rc" -ne 0 ]; then
  cat "$log"
  exit 1
fi
grep -v '^\[\(poll\|decode\|sdma\|ksym\|gputs\)' "$log" || true
if [ "${TIMING_DEBUG:-0}" = 1 ]; then
  echo "completion-signal timestamp reads: $(grep -c '^\[gputs' "$log" || true)," \
       "$(grep -c '^\[gputs.*read=FAILED' "$log" || true) failed," \
       "$(grep -c '^\[gputs.*start_ts=0 ' "$log" || true) with start_ts=0"
  grep '^\[gputs' "$log" | head -6 || true
  echo "retired on signal: $(grep -c 'completed via signal' "$log" || true)," \
       "on rptr after timeout: $(grep -c 'after signal timeout' "$log" || true)"
fi
sudo chmod a+r "$out"

# The shim announces itself from OnLoad. Built but silent means the HSA
# runtime did not load it, and no dispatch can carry GPU timestamps.
if [ -n "$shim" ] && ! grep -q '\[hsa-snoop-profiler\]' "$log"; then
  echo "::error::the HSA runtime did not load $shim"
  exit 1
fi
if [ -n "$shim" ] && ! grep -q 'completion signals attached' "$log"; then
  echo "::error::the profiler shim could not set up queue interception"
  exit 1
fi

dispatches=$(grep -c '"ev":"dispatch"' "$out" || true)
named=$(grep -c '"kernel":"v\(add\|scale\|dot\)(' "$out" || true)
with_ticks=$(grep -c '"gpu_start_ticks"' "$out" || true)
echo "captured $dispatches dispatches, $named resolved to a gfx-test kernel, $with_ticks with GPU timestamps"
if [ "$named" -eq 0 ]; then
  echo "::error::no dispatch resolved to a gfx-test kernel name"
  exit 1
fi
if [ "${TIMING_REQUIRE_SHIM:-0}" = 1 ] && [ "$with_ticks" -eq 0 ]; then
  echo "::error::no dispatch carried GPU timestamps"
  exit 1
fi
