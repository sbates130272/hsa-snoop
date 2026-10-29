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
set(SNAP_GFX "${SCRATCH_DIR}/prom-gfx.txt")
set(SNAP_SDMA "${SCRATCH_DIR}/prom-sdma.txt")
set(SNAP_AIS "${SCRATCH_DIR}/prom-ais.txt")
set(ORCHESTRATE_SCRIPT "${SCRATCH_DIR}/orchestrate.sh")

# Strategy:
#  Run three sequential hsa-snoop instances, each in -- child mode so that
#  process_vm_readv targets the correct PID and can read device-mapped ring
#  buffers that are inaccessible in --all / separate-process mode.
#
#  Phase 1 (gfx-test):  monotonicity scrapes + hsa_kernel_launches_total
#  Phase 2 (sdma-test): hsa_sdma_copies_total (child mode ensures ring is readable)
#  Phase 3 (ais-test):  ais_rx_bytes_total / ais_tx_bytes_total via AIS kprobe
#
#  After all three phases the relevant metric lines are merged into PROM_SNAPSHOT
#  for badge publishing.

set(SNAPSHOT_ARG_GFX "")
if(NOT PROM_SNAPSHOT STREQUAL "")
  set(SNAPSHOT_ARG_GFX "--prometheus-snapshot '${SNAP_GFX}'")
endif()

# Build optional phase command lines for the orchestrate script.
set(SDMA_PHASE "")
if(NOT SDMA_TEST STREQUAL "")
  set(SDMA_PHASE "
# ---- Phase 2: sdma-test (child mode for reliable SDMA ring reading) ----
'${HSA_SNOOP}' --prometheus --prometheus-port ${PROM_PORT} \
  --prometheus-snapshot '${SNAP_SDMA}' \
  --poll-us 500 \
  -- '${SDMA_TEST}' --streams 1 --buf-mb 1 --iters 50 --report 0 \
  >> \"\$SNOOP_LOG\" 2>&1
sdma_val=\$(grep -m1 '^hsa_sdma_copies_total ' '${SNAP_SDMA}' | awk '{print \$2}' 2>/dev/null || echo 0)
sdma_int=\${sdma_val%%.*}
if [ -z \"\$sdma_int\" ] || [ \"\$sdma_int\" -eq 0 ] 2>/dev/null; then
  echo 'ERROR: hsa_sdma_copies_total is 0 or absent after sdma-test' >&2
  exit 1
fi
echo \"hsa_sdma_copies_total=\$sdma_val (ok)\"
")
endif()

set(AIS_PHASE "")
if(NOT AIS_TEST STREQUAL "" AND NOT AIS_TARGET STREQUAL "")
  set(AIS_PHASE "
# ---- Phase 3: ais-test (child mode + --ais-snoop) ----
'${HSA_SNOOP}' --prometheus --prometheus-port ${PROM_PORT} \
  --prometheus-snapshot '${SNAP_AIS}' \
  --ais-snoop \
  --poll-us 500 \
  -- '${AIS_TEST}' --iters 10 --duration 10 '${AIS_TARGET}' \
  >> \"\$SNOOP_LOG\" 2>&1
rx_val=\$(grep -m1 '^ais_rx_bytes_total ' '${SNAP_AIS}' | awk '{print \$2}' 2>/dev/null || echo 0)
tx_val=\$(grep -m1 '^ais_tx_bytes_total ' '${SNAP_AIS}' | awk '{print \$2}' 2>/dev/null || echo 0)
rx_int=\${rx_val%%.*}; tx_int=\${tx_val%%.*}
if { [ -z \"\$rx_int\" ] || [ \"\$rx_int\" -eq 0 ] 2>/dev/null; } &&
   { [ -z \"\$tx_int\" ] || [ \"\$tx_int\" -eq 0 ] 2>/dev/null; }; then
  echo 'ERROR: both ais_rx_bytes_total and ais_tx_bytes_total are 0 or absent after ais-test' >&2
  exit 1
fi
echo \"ais_rx_bytes_total=\$rx_val ais_tx_bytes_total=\$tx_val (ok)\"
")
endif()

# Build the merge fragment: combine metric lines from each phase snapshot.
set(MERGE_SNAPSHOT "")
if(NOT PROM_SNAPSHOT STREQUAL "")
  set(MERGE_SNAPSHOT "
# ---- Merge metric lines from each phase into the final snapshot ----
: > '${PROM_SNAPSHOT}'
[ -f '${SNAP_GFX}' ]  && grep '^hsa_kernel_launches_total '  '${SNAP_GFX}'  >> '${PROM_SNAPSHOT}' || true
[ -f '${SNAP_SDMA}' ] && grep '^hsa_sdma_copies_total '      '${SNAP_SDMA}' >> '${PROM_SNAPSHOT}' || true
[ -f '${SNAP_AIS}' ]  && grep '^ais_rx_bytes_total ' '${SNAP_AIS}' >> '${PROM_SNAPSHOT}' || true
[ -f '${SNAP_AIS}' ]  && grep '^ais_tx_bytes_total ' '${SNAP_AIS}' >> '${PROM_SNAPSHOT}' || true
")
endif()

file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
set -e
HSA_SNOOP='${HSA_SNOOP}'
SNOOP_LOG='${SNOOP_LOG}'
PROM_URL='${PROM_URL}'
S1='${SCRAPE1}'
S2='${SCRAPE2}'
S3='${SCRAPE3}'

rm -f \"\$SNOOP_LOG\" \"\$S1\" \"\$S2\" \"\$S3\"

# ---- Phase 1: gfx-test in child mode for monotonicity scrapes ----
#  hsa-snoop wraps gfx-test directly, so process_vm_readv uses gfx-test's
#  PID and can read its host-pinned AQL ring buffers throughout the run.
\"\$HSA_SNOOP\" --prometheus --prometheus-port ${PROM_PORT} \
  ${SNAPSHOT_ARG_GFX} \
  --poll-us 500 \
  -- '${GFX_TEST}' --elements 128 --iters 1 --batch 3 --loops 20 --sleep-ms 250 \
  > \"\$SNOOP_LOG\" 2>&1 &
SNOOP_PID=\$!

# Wait for prometheus endpoint (up to 15 s).
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

# Collect up to 3 scrapes while gfx-test is running (hsa-snoop still alive).
SCRAPED=0
for _i in 1 2 3 4 5 6 7 8 9 10; do
  sleep 1
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

${SDMA_PHASE}
${AIS_PHASE}
${MERGE_SNAPSHOT}
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
