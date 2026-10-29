if(NOT DEFINED HSA_SNOOP OR NOT DEFINED AIS_TEST OR
   NOT DEFINED TRACE_OUTPUT OR NOT DEFINED AIS_TARGET)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "ais-write-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

# Run hsa-snoop wrapping ais-test --write (VRAM->file) and verify that
# AIS write events appear in the JSON trace.
file(REMOVE "${TRACE_OUTPUT}")

execute_process(
  COMMAND "${HSA_SNOOP}"
    --format json
    --out "${TRACE_OUTPUT}"
    --ais-snoop
    --poll-us 1000
    -- "${AIS_TEST}" --write --iters 10 --duration 10 "${AIS_TARGET}"
  RESULT_VARIABLE snoop_result
  OUTPUT_VARIABLE snoop_stdout
  ERROR_VARIABLE  snoop_stderr
  TIMEOUT 60)
if(NOT snoop_result EQUAL 0)
  message(FATAL_ERROR
    "hsa-snoop ais write run failed (${snoop_result})\n"
    "stdout:\n${snoop_stdout}\n"
    "stderr:\n${snoop_stderr}")
endif()

if(NOT EXISTS "${TRACE_OUTPUT}")
  message(FATAL_ERROR "hsa-snoop did not create ${TRACE_OUTPUT}")
endif()
file(READ "${TRACE_OUTPUT}" trace_json)

string(REGEX MATCHALL "\"name\":\"ais/write" ais_write_events "${trace_json}")
list(LENGTH ais_write_events write_count)
if(write_count EQUAL 0)
  message(FATAL_ERROR
    "no AIS write events decoded in the trace\n${snoop_stderr}")
endif()

message(STATUS "AIS write: decoded ${write_count} AIS write event(s)")
