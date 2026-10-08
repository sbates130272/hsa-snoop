if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR NOT DEFINED TRACE_DIR)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "all-mode-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

file(MAKE_DIRECTORY "${TRACE_DIR}")
file(GLOB _old_traces "${TRACE_DIR}/*.json" "${TRACE_DIR}/*.pftrace")
foreach(_f IN LISTS _old_traces)
  file(REMOVE "${_f}")
endforeach()

set(SNOOP_LOG "${TRACE_DIR}/snoop.log")
set(ORCHESTRATE_SCRIPT "${TRACE_DIR}/orchestrate.sh")

# Strategy:
#  1. Start hsa-snoop --all in background (with --duration 60 as a safety net).
#  2. Poll until "discovery armed" appears in the log (kprobe is live).
#  3. Launch two concurrent gfx-test processes and wait for them to finish.
#  4. Give the parser 3 s to flush, then SIGTERM hsa-snoop (faster than waiting
#     for the full --duration 60).
#  5. Wait for hsa-snoop to exit and assert trace files with kernel events.
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
set -e
LOG='${SNOOP_LOG}'
TRACE_DIR='${TRACE_DIR}'
HSA_SNOOP='${HSA_SNOOP}'
GFX_TEST='${GFX_TEST}'

rm -f \"\$LOG\"

# 1. Start hsa-snoop --all.
\"\$HSA_SNOOP\" --all --format json --out-dir \"\$TRACE_DIR\" \
  --poll-us 1000 --duration 60 > \"\$LOG\" 2>&1 &
SNOOP_PID=\$!
echo \"hsa-snoop PID=\$SNOOP_PID\" >&2

# 2. Poll for 'discovery armed' (up to 15 s).
n=0
while [ \"\$n\" -lt 30 ]; do
  sleep 0.5
  if grep -q 'discovery armed' \"\$LOG\" 2>/dev/null; then
    echo 'kprobe armed' >&2
    break
  fi
  n=\$((n + 1))
done
if ! grep -q 'discovery armed' \"\$LOG\" 2>/dev/null; then
  echo 'ERROR: hsa-snoop did not arm in time' >&2
  cat \"\$LOG\" >&2
  kill \$SNOOP_PID 2>/dev/null || true
  exit 1
fi

# 3. Launch two concurrent gfx-test processes and wait for both.
\"\$GFX_TEST\" --elements 128 --iters 1 --loops 2 --batch 3 --sleep-ms 500 &
P1=\$!
\"\$GFX_TEST\" --elements 128 --iters 1 --loops 2 --batch 3 --sleep-ms 500 &
P2=\$!
echo \"gfx-test PIDs: \$P1 \$P2\" >&2
wait \$P1
wait \$P2
echo 'gfx-test processes done' >&2

# 4. Give the parser time to process final queue drain, then terminate hsa-snoop.
sleep 3
kill \$SNOOP_PID 2>/dev/null || true
wait \$SNOOP_PID 2>/dev/null || true
echo 'hsa-snoop done' >&2
")
execute_process(COMMAND chmod +x "${ORCHESTRATE_SCRIPT}")

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE orch_result
  OUTPUT_VARIABLE orch_stdout
  ERROR_VARIABLE orch_stderr
  TIMEOUT 150)

if(NOT orch_result EQUAL 0)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "orchestration script failed (${orch_result})\n"
    "stderr:\n${orch_stderr}\n"
    "hsa-snoop log:\n${_log}")
endif()

file(GLOB trace_files "${TRACE_DIR}/*.json")
list(LENGTH trace_files trace_count)

if(trace_count EQUAL 0)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "no trace files in ${TRACE_DIR}; hsa-snoop --all produced no output\n"
    "hsa-snoop log:\n${_log}")
endif()

set(total_kernel_events 0)
foreach(trace_file IN LISTS trace_files)
  file(READ "${trace_file}" tj)
  string(REGEX MATCHALL "\"name\":\"kernel_dispatch\"" named "${tj}")
  string(REGEX MATCHALL "\"name\":\"kernel_0x[0-9a-f]+" addr "${tj}")
  list(LENGTH named n1)
  list(LENGTH addr n2)
  math(EXPR kevt "${n1} + ${n2}")
  # In --all mode hsa-snoop writes one trace file per queue; small command
  # queues (64 slots) carry no kernel dispatches, so only assert that the
  # total across all files is non-zero.
  math(EXPR total_kernel_events "${total_kernel_events} + ${kevt}")
endforeach()

if(total_kernel_events EQUAL 0)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "no kernel events found across any of the ${trace_count} trace files\n"
    "hsa-snoop log:\n${_log}")
endif()

message(STATUS
  "all-mode-hardware-test: ${trace_count} trace file(s), "
  "${total_kernel_events} kernel event(s) total")
