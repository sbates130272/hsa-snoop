if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR NOT DEFINED SCRATCH_DIR)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "prometheus-monotonicity-hardware-test must run as root; "
    "use sudo ctest --test-dir <build> -L hardware")
endif()

execute_process(
  COMMAND curl --version
  RESULT_VARIABLE curl_result
  OUTPUT_QUIET ERROR_QUIET)
if(NOT curl_result EQUAL 0)
  message(FATAL_ERROR "curl is required for prometheus-monotonicity-hardware-test")
endif()

file(MAKE_DIRECTORY "${SCRATCH_DIR}")

set(PROM_PORT "9489")
set(PROM_URL "http://127.0.0.1:${PROM_PORT}/metrics")
set(SNOOP_LOG "${SCRATCH_DIR}/snoop.log")
set(SCRAPE1 "${SCRATCH_DIR}/scrape_1.txt")
set(SCRAPE2 "${SCRATCH_DIR}/scrape_2.txt")
set(SCRAPE3 "${SCRATCH_DIR}/scrape_3.txt")
set(ORCHESTRATE_SCRIPT "${SCRATCH_DIR}/orchestrate.sh")

# Strategy:
#  1. Start hsa-snoop --all --prometheus in background; wait for it to arm.
#  2. Run 3 sequential gfx-test invocations (small, fast: ~0.8 s each).
#     After each one, immediately scrape the prometheus endpoint.
#     The endpoint stays alive between gfx-test runs because --all mode
#     does not exit when the observed process finishes.
#  3. Assert monotonic hsa_kernel_launches_total across the 3 scrapes.
#
# Using sequential gfx-test runs (not concurrent) avoids the parallel-GPU
# scheduler bottleneck on the rocjitsu emulator.  Each run fires the kprobe
# fresh so hsa-snoop increments the counter on each invocation.
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
HSA_SNOOP='${HSA_SNOOP}'
GFX_TEST='${GFX_TEST}'
SNOOP_LOG='${SNOOP_LOG}'
PROM_URL='${PROM_URL}'
S1='${SCRAPE1}'
S2='${SCRAPE2}'
S3='${SCRAPE3}'

rm -f \"\$SNOOP_LOG\"

# 1. Start hsa-snoop --all --prometheus.
\"\$HSA_SNOOP\" --all --prometheus --prometheus-port ${PROM_PORT} \
  --poll-us 500 > \"\$SNOOP_LOG\" 2>&1 &
SNOOP_PID=\$!

# 2. Wait for kprobe armed (up to 15 s).
n=0
while [ \"\$n\" -lt 30 ]; do
  sleep 0.5
  grep -q 'discovery armed' \"\$SNOOP_LOG\" 2>/dev/null && break
  n=\$((n + 1))
done
grep -q 'discovery armed' \"\$SNOOP_LOG\" 2>/dev/null || {
  echo 'ERROR: hsa-snoop did not arm' >&2; kill \$SNOOP_PID; exit 1
}

# 3a. First gfx-test run; scrape once it finishes.
\"\$GFX_TEST\" --elements 128 --iters 1 --batch 3 --loops 1 >/dev/null 2>&1
curl -sf \"\$PROM_URL\" > \"\$S1\" 2>/dev/null || true

# 3b. Second gfx-test run; scrape.
\"\$GFX_TEST\" --elements 128 --iters 1 --batch 3 --loops 1 >/dev/null 2>&1
curl -sf \"\$PROM_URL\" > \"\$S2\" 2>/dev/null || true

# 3c. Third gfx-test run; scrape.
\"\$GFX_TEST\" --elements 128 --iters 1 --batch 3 --loops 1 >/dev/null 2>&1
curl -sf \"\$PROM_URL\" > \"\$S3\" 2>/dev/null || true

# 4. Kill hsa-snoop.
kill \$SNOOP_PID 2>/dev/null || true
wait \$SNOOP_PID 2>/dev/null || true
")
execute_process(COMMAND chmod +x "${ORCHESTRATE_SCRIPT}")

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE orch_result
  OUTPUT_VARIABLE orch_stdout
  ERROR_VARIABLE orch_stderr
  TIMEOUT 90)

if(NOT orch_result EQUAL 0)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "orchestration failed (${orch_result})\n"
    "stderr:\n${orch_stderr}\n"
    "hsa-snoop log:\n${_log}")
endif()

set(scrape_files "${SCRAPE1}" "${SCRAPE2}" "${SCRAPE3}")

set(scrapes_found 0)
foreach(f IN LISTS scrape_files)
  if(EXISTS "${f}")
    file(SIZE "${f}" fsz)
    if(fsz GREATER 0)
      math(EXPR scrapes_found "${scrapes_found} + 1")
    endif()
  endif()
endforeach()

if(scrapes_found EQUAL 0)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "no prometheus scrapes collected\n"
    "hsa-snoop log:\n${_log}")
endif()

set(prev_value -1)
set(all_values "")
set(idx 0)
foreach(f IN LISTS scrape_files)
  math(EXPR idx "${idx} + 1")
  if(NOT EXISTS "${f}")
    continue()
  endif()
  file(READ "${f}" scrape_text)

  string(REGEX MATCH
    "hsa_kernel_launches_total[^\n]* ([0-9]+)[^\n]*\n"
    _match "${scrape_text}")
  if(_match)
    set(val "${CMAKE_MATCH_1}")
    list(APPEND all_values "${val}")
    if(prev_value GREATER val)
      message(FATAL_ERROR
        "counter regressed: ${prev_value} -> ${val} at scrape ${idx}")
    endif()
    set(prev_value "${val}")
  endif()
endforeach()

list(LENGTH all_values value_count)
if(value_count LESS 2)
  if(EXISTS "${SNOOP_LOG}")
    file(READ "${SNOOP_LOG}" _log)
  else()
    set(_log "(no log)")
  endif()
  message(FATAL_ERROR
    "fewer than 2 scrapes had hsa_kernel_launches_total (got ${value_count})\n"
    "hsa-snoop log:\n${_log}")
endif()

list(GET all_values -1 final_val)
if(final_val EQUAL 0)
  message(FATAL_ERROR "hsa_kernel_launches_total was 0 in the final scrape")
endif()

set(up_found 0)
foreach(f IN LISTS scrape_files)
  if(NOT EXISTS "${f}")
    continue()
  endif()
  file(READ "${f}" scrape_text)
  if(scrape_text MATCHES "hsa_snoop_up[^\n]* 1")
    set(up_found 1)
    break()
  endif()
endforeach()
if(NOT up_found)
  message(FATAL_ERROR "hsa_snoop_up was never 1 in any scrape")
endif()

message(STATUS
  "prometheus-monotonicity-hardware-test: ${value_count} scrape(s), "
  "final hsa_kernel_launches_total=${final_val} (monotonic)")
