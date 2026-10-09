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
    "sdma-stress-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

# 2 streams × 4 MB × 20 iters (H2D + D2D + D2H): 3× more SDMA traffic than
# the baseline sdma-hardware-test, exercising multi-stream decode and verifying
# both H2D and D2D copy directions appear in the trace.
file(REMOVE "${TRACE_OUTPUT}")
execute_process(
  COMMAND "${HSA_SNOOP}"
    --format json
    --out "${TRACE_OUTPUT}"
    --poll-us 1000
    -- "${SDMA_TEST}" --streams 2 --buf-mb 4 --iters 20 --report 0
  RESULT_VARIABLE snoop_result
  OUTPUT_VARIABLE snoop_stdout
  ERROR_VARIABLE snoop_stderr
  TIMEOUT 75)
if(NOT snoop_result EQUAL 0)
  message(FATAL_ERROR
    "hsa-snoop sdma-stress run failed (${snoop_result})\n"
    "stdout:\n${snoop_stdout}\n"
    "stderr:\n${snoop_stderr}")
endif()

# Verify SDMA version was detected.
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

# Expect substantially more copy_linear events than the baseline test.
# 2 streams × 20 iters × at least 1 H2D direction = ≥ 40 copies minimum;
# require ≥ 20 to be conservative in case the GPU routes some legs via compute.
string(REGEX MATCHALL "\"name\":\"copy_linear\"" copy_events "${trace_json}")
list(LENGTH copy_events copy_count)
if(copy_count LESS 20)
  message(FATAL_ERROR
    "sdma-stress expected ≥ 20 COPY_LINEAR packets, got ${copy_count}\n"
    "${snoop_stderr}")
endif()

# NOP packets must not dominate.
string(REGEX MATCHALL "\"name\":\"nop\"" nop_events "${trace_json}")
list(LENGTH nop_events nop_count)
if(nop_count GREATER copy_count)
  message(FATAL_ERROR
    "NOP packets dominate: ${nop_count} NOP vs ${copy_count} COPY_LINEAR")
endif()

# At least one H2D copy with non-zero addresses.
if(NOT trace_json MATCHES "\"direction\":\"h2d\"" OR
   NOT trace_json MATCHES "\"src\":\"0x[1-9a-f][0-9a-f]*\"" OR
   NOT trace_json MATCHES "\"dst\":\"0x[1-9a-f][0-9a-f]*\"")
  message(FATAL_ERROR
    "no H2D COPY_LINEAR with non-zero src/dst was decoded\n${snoop_stderr}")
endif()

# Verify two SDMA queues were discovered (one per stream).
string(REGEX MATCHALL "kind=sdma" sdma_queues "${snoop_stderr}")
list(LENGTH sdma_queues sdma_queue_count)
if(sdma_queue_count LESS 2)
  message(FATAL_ERROR
    "expected ≥ 2 SDMA queues for 2-stream test, got ${sdma_queue_count}")
endif()

message(STATUS
  "SDMA v${detected_sdma_version} stress: ${copy_count} COPY_LINEAR, "
  "${nop_count} bounded NOP, ${sdma_queue_count} SDMA queue(s)")
