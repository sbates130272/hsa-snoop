if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR NOT DEFINED SCRATCH_DIR)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()
if(NOT DEFINED PROM_SNAPSHOT)
  set(PROM_SNAPSHOT "")
endif()
if(NOT DEFINED SDMA_TEST)
  set(SDMA_TEST "")
endif()
if(NOT DEFINED AIS_TEST)
  set(AIS_TEST "")
endif()
if(NOT DEFINED AIS_TARGET)
  set(AIS_TARGET "")
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

# Hard-fail if AIS is requested but the NVMe target file is missing.
# The CI VM always has an emulated NVMe; absence indicates a setup regression.
if(NOT AIS_TEST STREQUAL "" AND NOT AIS_TARGET STREQUAL "")
  if(NOT EXISTS "${AIS_TARGET}")
    message(FATAL_ERROR
      "AIS_TARGET '${AIS_TARGET}' not found — NVMe setup must succeed before this test runs")
  endif()
endif()

file(MAKE_DIRECTORY "${SCRATCH_DIR}")

set(PROM_PORT "9489")
set(PROM_URL "http://127.0.0.1:${PROM_PORT}/metrics")
set(SNOOP_LOG "${SCRATCH_DIR}/snoop.log")
set(SCRAPE1 "${SCRATCH_DIR}/scrape_1.txt")
set(SCRAPE2 "${SCRATCH_DIR}/scrape_2.txt")
set(SCRAPE3 "${SCRATCH_DIR}/scrape_3.txt")
set(ORCHESTRATE_SCRIPT "${SCRATCH_DIR}/orchestrate.sh")

# Build the --ais-snoop flag conditionally.
set(AIS_SNOOP_ARG "")
if(NOT AIS_TEST STREQUAL "")
  set(AIS_SNOOP_ARG "--ais-snoop")
endif()

# Strategy:
#  Run hsa-snoop in --all daemon mode (not -- child mode) so that each
#  workload binary is tracked by its own PID.  In -- child mode, hsa-snoop
#  uses the child's PID for process_vm_readv; a shell wrapper makes the GPU
#  workloads grandchildren whose memory hsa-snoop cannot read.
#
#  Sequence:
#   1. Start hsa-snoop --all --prometheus in background.
#   2. Wait for the /metrics endpoint.
#   3. Run gfx-test in background; collect monotonicity scrapes while it runs.
#   4. Wait for gfx-test to exit.
#   5. Run sdma-test (if supplied), then ais-test (if supplied), sequentially.
#   6. SIGINT hsa-snoop so it writes the snapshot and exits cleanly.
#   7. Verify SDMA and AIS counters in the snapshot.
set(SNAPSHOT_ARG "")
if(NOT PROM_SNAPSHOT STREQUAL "")
  set(SNAPSHOT_ARG "--prometheus-snapshot '${PROM_SNAPSHOT}'")
endif()

# Build optional workload command lines for the orchestrate script.
set(SDMA_CMD "")
if(NOT SDMA_TEST STREQUAL "")
  set(SDMA_CMD "'${SDMA_TEST}' --streams 1 --buf-mb 1 --iters 20 --report 0")
endif()
set(AIS_CMD "")
if(NOT AIS_TEST STREQUAL "" AND NOT AIS_TARGET STREQUAL "")
  set(AIS_CMD "'${AIS_TEST}' --iters 10 --duration 10 '${AIS_TARGET}'")
endif()

# Snapshot verification shell fragments — embedded into the orchestrate script.
set(VERIFY_SDMA "")
if(NOT SDMA_TEST STREQUAL "")
  set(VERIFY_SDMA "
# Verify sdma_copies is non-zero in the snapshot.
if [ -f '${PROM_SNAPSHOT}' ]; then
  sdma_val=\$(grep -m1 '^hsa_sdma_copies_total ' '${PROM_SNAPSHOT}' | awk '{print \$2}' || echo 0)
  sdma_int=\${sdma_val%%.*}
  if [ -z \"\$sdma_int\" ] || [ \"\$sdma_int\" -eq 0 ] 2>/dev/null; then
    echo 'ERROR: hsa_sdma_copies_total is 0 or absent in snapshot' >&2
    exit 1
  fi
  echo \"hsa_sdma_copies_total=\$sdma_val (ok)\"
fi
")
endif()
set(VERIFY_AIS "")
if(NOT AIS_TEST STREQUAL "")
  set(VERIFY_AIS "
# Verify ais bytes are non-zero in the snapshot.
if [ -f '${PROM_SNAPSHOT}' ]; then
  rx_val=\$(grep -m1 '^ais_rx_bytes_total ' '${PROM_SNAPSHOT}' | awk '{print \$2}' || echo 0)
  tx_val=\$(grep -m1 '^ais_tx_bytes_total ' '${PROM_SNAPSHOT}' | awk '{print \$2}' || echo 0)
  rx_int=\${rx_val%%.*}; tx_int=\${tx_val%%.*}
  if { [ -z \"\$rx_int\" ] || [ \"\$rx_int\" -eq 0 ] 2>/dev/null; } &&
     { [ -z \"\$tx_int\" ] || [ \"\$tx_int\" -eq 0 ] 2>/dev/null; }; then
    echo 'ERROR: both ais_rx_bytes_total and ais_tx_bytes_total are 0 or absent in snapshot' >&2
    exit 1
  fi
  echo \"ais_rx_bytes_total=\$rx_val ais_tx_bytes_total=\$tx_val (ok)\"
fi
")
endif()

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

# 1. Start hsa-snoop in --all daemon mode so every workload binary is
#    tracked by its own PID.  Using -- child mode with a wrapper script
#    makes workloads grandchildren; process_vm_readv only works on the
#    direct child PID and would see 0 queues.
\"\$HSA_SNOOP\" --all --prometheus --prometheus-port ${PROM_PORT} \
  ${SNAPSHOT_ARG} \
  ${AIS_SNOOP_ARG} \
  --poll-us 500 \
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

# 3. Run gfx-test in background; collect up to 3 scrapes while it runs.
#    --loops 20 --sleep-ms 250 gives ~5 s of active dispatch time.
\"\$GFX_TEST\" --elements 128 --iters 1 --batch 3 --loops 20 --sleep-ms 250 &
GFX_PID=\$!

SCRAPED=0
for _i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
  kill -0 \"\$GFX_PID\" 2>/dev/null || break
  _out=''
  if [ \"\$SCRAPED\" -eq 0 ]; then _out=\"\$S1\"; fi
  if [ \"\$SCRAPED\" -eq 1 ]; then _out=\"\$S2\"; fi
  if [ \"\$SCRAPED\" -eq 2 ]; then _out=\"\$S3\"; fi
  if [ -n \"\$_out\" ]; then
    curl -sf \"\$PROM_URL\" > \"\$_out\" 2>/dev/null && SCRAPED=\$((SCRAPED + 1))
  fi
  [ \"\$SCRAPED\" -ge 3 ] && break
done
wait \$GFX_PID 2>/dev/null || true

# 4. Run sdma-test and ais-test sequentially (foreground) so hsa-snoop
#    tracks each by its own PID via the system-wide --all monitor.
${SDMA_CMD}
${AIS_CMD}

# 5. Signal hsa-snoop to write the prometheus snapshot and exit cleanly.
kill -INT \$SNOOP_PID 2>/dev/null
wait \$SNOOP_PID 2>/dev/null || true

# 6. Verify SDMA and AIS counts in the snapshot.
${VERIFY_SDMA}
${VERIFY_AIS}
")
execute_process(COMMAND chmod +x "${ORCHESTRATE_SCRIPT}")

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE orch_result
  OUTPUT_VARIABLE orch_stdout
  ERROR_VARIABLE orch_stderr
  TIMEOUT 120)

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

set(extra_info "")
if(NOT SDMA_TEST STREQUAL "")
  string(APPEND extra_info " sdma-test=yes")
endif()
if(NOT AIS_TEST STREQUAL "")
  string(APPEND extra_info " ais-test=yes")
endif()

message(STATUS
  "prometheus-monotonicity-hardware-test: ${value_count} scrape(s), "
  "final hsa_kernel_launches_total=${final_val} (monotonic)${extra_info}")
