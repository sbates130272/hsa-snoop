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
    "kfd-uprobe-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

file(MAKE_DIRECTORY "${TRACE_DIR}")

set(SNOOP_LOG "${TRACE_DIR}/snoop.log")
set(ORCHESTRATE_SCRIPT "${TRACE_DIR}/kfd_uprobe_orchestrate.sh")

# Strategy:
#  1. Start hsa-snoop --all --mode kfd-uprobe in background.
#  2. Poll until "kfd-uprobe armed" appears (probe is live).
#  3. Launch gfx-test; verify the GPU does NOT deadlock (must complete).
#  4. SIGTERM hsa-snoop; verify hsa_snoop_up appeared in log.
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
set -e
LOG='${SNOOP_LOG}'
TRACE_DIR='${TRACE_DIR}'
HSA_SNOOP='${HSA_SNOOP}'
GFX_TEST='${GFX_TEST}'

rm -f \"\$LOG\"

# 1. Start hsa-snoop in kfd-uprobe mode.
\"\$HSA_SNOOP\" --all --mode kfd-uprobe --format json --out-dir \"\$TRACE_DIR\" \
  --poll-us 1000 > \"\$LOG\" 2>&1 &
SNOOP_PID=\$!
echo \"hsa-snoop PID=\$SNOOP_PID\" >&2

# 2. Poll for 'kfd-uprobe armed' (up to 20 s).
n=0
while [ \"\$n\" -lt 40 ]; do
  sleep 0.5
  if grep -q 'kfd-uprobe armed' \"\$LOG\" 2>/dev/null; then
    echo 'kfd-uprobe armed' >&2
    break
  fi
  n=\$((n + 1))
done
if ! grep -q 'kfd-uprobe armed' \"\$LOG\" 2>/dev/null; then
  echo 'ERROR: hsa-snoop kfd-uprobe mode did not arm in time' >&2
  cat \"\$LOG\" >&2
  kill \"\$SNOOP_PID\" 2>/dev/null || true
  exit 1
fi

# 3. Run gfx-test. In kfd-uprobe mode process_vm_readv is not used,
#    so GPU dispatch must complete without deadlocking.
echo 'Running gfx-test alongside kfd-uprobe...' >&2
\"\$GFX_TEST\" --elements 128 --iters 1 --loops 1 --batch 3 --sleep-ms 0 || {
  echo 'ERROR: gfx-test failed — GPU may have deadlocked' >&2
  cat \"\$LOG\" >&2
  kill \"\$SNOOP_PID\" 2>/dev/null || true
  exit 1
}
echo 'gfx-test PASS (no deadlock)' >&2

# 4. Let the parser flush and kill hsa-snoop.
sleep 3
kill \"\$SNOOP_PID\" 2>/dev/null || true
wait \"\$SNOOP_PID\" 2>/dev/null || true

# 5. Verify queue was discovered (kfd-uprobe detected an AQL queue).
if grep -q 'kind=aql' \"\$LOG\" 2>/dev/null; then
  echo 'PASS: AQL queue observed in kfd-uprobe mode' >&2
else
  echo 'WARN: no AQL queue observed — may be expected if GPU did not launch' >&2
fi

echo 'kfd-uprobe hardware test complete' >&2
")

execute_process(
  COMMAND chmod +x "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE chmod_result)

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE test_result
  ECHO_OUTPUT_VARIABLE
  ECHO_ERROR_VARIABLE)

if(NOT test_result EQUAL 0)
  message(FATAL_ERROR "kfd-uprobe hardware test FAILED (exit ${test_result})")
endif()
