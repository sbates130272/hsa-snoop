if(NOT DEFINED HSA_SNOOP OR NOT DEFINED SDMA_TEST OR
   NOT DEFINED TRACE_OUTPUT)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()
if(NOT DEFINED EXPECTED_SDMA_VERSION)
  set(EXPECTED_SDMA_VERSION "AUTO")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "sdma-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

file(REMOVE "${TRACE_OUTPUT}")
execute_process(
  COMMAND "${HSA_SNOOP}"
    --format json
    --out "${TRACE_OUTPUT}"
    --poll-us 1000
    -- "${SDMA_TEST}" --streams 1 --buf-mb 1 --iters 20 --report 0
  RESULT_VARIABLE snoop_result
  OUTPUT_VARIABLE snoop_stdout
  ERROR_VARIABLE snoop_stderr
  TIMEOUT 45)
if(NOT snoop_result EQUAL 0)
  message(FATAL_ERROR
    "hsa-snoop hardware run failed (${snoop_result})\n"
    "stdout:\n${snoop_stdout}\n"
    "stderr:\n${snoop_stderr}")
endif()

string(REGEX MATCH "kind=sdma[^\n]*sdma=v([0-9]+)" detected_sdma_line
       "${snoop_stderr}")
if(NOT detected_sdma_line)
  message(FATAL_ERROR
    "no supported SDMA generation was detected\n${snoop_stderr}")
endif()
set(detected_sdma_version "${CMAKE_MATCH_1}")
if(NOT "${EXPECTED_SDMA_VERSION}" STREQUAL "AUTO" AND
   NOT "${EXPECTED_SDMA_VERSION}" STREQUAL "${detected_sdma_version}")
  message(FATAL_ERROR
    "expected SDMA v${EXPECTED_SDMA_VERSION}, but detected "
    "v${detected_sdma_version}\n${snoop_stderr}")
endif()

if(NOT EXISTS "${TRACE_OUTPUT}")
  message(FATAL_ERROR "hsa-snoop did not create ${TRACE_OUTPUT}")
endif()
file(READ "${TRACE_OUTPUT}" trace_json)

string(REGEX MATCHALL "\"name\":\"copy_linear\"" copy_events
       "${trace_json}")
list(LENGTH copy_events copy_count)
if(copy_count EQUAL 0)
  message(FATAL_ERROR
    "no COPY_LINEAR packet was decoded\n${snoop_stderr}")
endif()

string(REGEX MATCHALL "\"name\":\"nop\"" nop_events "${trace_json}")
list(LENGTH nop_events nop_count)
if(nop_count GREATER copy_count)
  message(FATAL_ERROR
    "NOP packets dominate the SDMA decode: ${nop_count} NOP vs "
    "${copy_count} COPY_LINEAR")
endif()

if(NOT trace_json MATCHES "\"bytes\":1048576")
  message(FATAL_ERROR
    "no COPY_LINEAR packet with the expected 1 MiB COUNT was decoded")
endif()

# HIP is free to implement individual D2D/D2H operations with compute kernels
# instead of SDMA, so do not require every requested direction to appear on an
# SDMA queue. Non-zero decoded addresses still validate the COPY_LINEAR field
# positions, and this workload reliably submits at least its H2D leg via SDMA.
if(NOT trace_json MATCHES "\"direction\":\"h2d\"" OR
   NOT trace_json MATCHES "\"src\":\"0x[1-9a-f][0-9a-f]*\"" OR
   NOT trace_json MATCHES "\"dst\":\"0x[1-9a-f][0-9a-f]*\"")
  message(FATAL_ERROR
    "no H2D COPY_LINEAR packet with non-zero src/dst was decoded")
endif()

message(STATUS
  "SDMA v${detected_sdma_version}: decoded ${copy_count} COPY_LINEAR and "
  "${nop_count} bounded NOP packets")

# An SDMA queue whose decoder met a packet it does not recognise skips to the
# write pointer and loses everything queued behind it. That is a decoder
# defect for this GPU, so fail on it instead of passing on a partial count.
string(REGEX MATCHALL "\\[summary\\] sdma queue[^\n]*" sdma_summaries "${snoop_stderr}")
foreach(_summary IN LISTS sdma_summaries)
  if(NOT _summary MATCHES "skipped_dwords=0 " OR
     NOT _summary MATCHES "undecoded_dwords=0")
    message(FATAL_ERROR
      "SDMA ring was not fully decoded: ${_summary}\n${snoop_stderr}")
  endif()
endforeach()

# Per-direction split and hsa-snoop's own log (queue and summary lines), so a
# run with an unusual copy count can be explained from the test output alone.
foreach(_dir h2d d2h d2d h2h unknown)
  string(REGEX MATCHALL "\"direction\":\"${_dir}\"" _dir_events "${trace_json}")
  list(LENGTH _dir_events _dir_count)
  list(APPEND dir_summary "${_dir}=${_dir_count}")
endforeach()
string(REPLACE ";" " " dir_summary "${dir_summary}")
message(STATUS "SDMA copy directions: ${dir_summary}")
message(STATUS "hsa-snoop log:\n${snoop_stderr}")
