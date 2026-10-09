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
#  Run hsa-snoop --prometheus wrapping a long-running gfx-test workload (many
#  iterations / loops so it stays alive for several seconds).  Concurrently
#  poll /metrics every second and save up to 3 non-empty scrapes taken while
#  gfx-test is still running.  hsa-snoop exits naturally when gfx-test ends.
#
#  This avoids the eviction-before-scrape race: because gfx-test is a child of
#  hsa-snoop (not a separate process), the parser can read the AQL ring while
#  the process is alive, ensuring hsa_kernel_launches_total increments before
#  the scrapes are collected.
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
HSA_SNOOP='${HSA_SNOOP}'
GFX_TEST='${GFX_TEST}'
SNOOP_LOG='${SNOOP_LOG}'
PROM_URL='${PROM_URL}'
S1='${SCRAPE1}'
S2='${SCRAPE2}'
S3='${SCRAPE3}'

rm -f \"\$SNOOP_LOG\" \"\$S1\" \"\$S2\" \"\$S3\"

# 1. Start hsa-snoop wrapping a gfx-test workload long enough for scrapes.
#    --loops 20 --sleep-ms 250 gives ~5+ seconds of active dispatch time,
#    enough for 3 or more 1-second scrapes to land while the process is alive.
\"\$HSA_SNOOP\" --prometheus --prometheus-port ${PROM_PORT} \
  --poll-us 500 \
  -- \"\$GFX_TEST\" --elements 128 --iters 1 --batch 3 --loops 20 \
                   --sleep-ms 250 \
  > \"\$SNOOP_LOG\" 2>&1 &
SNOOP_PID=\$!

# 2. Wait for prometheus endpoint to become reachable (up to 15 s).
n=0
while [ \"\$n\" -lt 30 ]; do
  sleep 0.5
  curl -sf \"\$PROM_URL\" -o /dev/null 2>/dev/null && break
  n=\$((n + 1))
done
curl -sf \"\$PROM_URL\" -o /dev/null 2>/dev/null || {
  echo 'ERROR: prometheus endpoint did not come up' >&2
  kill \$SNOOP_PID 2>/dev/null; wait \$SNOOP_PID 2>/dev/null
  cat \"\$SNOOP_LOG\" >&2
  exit 1
}

# 3. Collect up to 3 scrapes while hsa-snoop is running; one per second.
SCRAPED=0
for _i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
  # Stop collecting once hsa-snoop exits (gfx-test done).
  kill -0 \"\$SNOOP_PID\" 2>/dev/null || break
  _out=''
  if [ \"\$SCRAPED\" -eq 0 ]; then _out=\"\$S1\"; fi
  if [ \"\$SCRAPED\" -eq 1 ]; then _out=\"\$S2\"; fi
  if [ \"\$SCRAPED\" -eq 2 ]; then _out=\"\$S3\"; fi
  if [ -n \"\$_out\" ]; then
    curl -sf \"\$PROM_URL\" > \"\$_out\" 2>/dev/null && SCRAPED=\$((SCRAPED + 1))
  fi
  [ \"\$SCRAPED\" -ge 3 ] && break
done

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
