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
    "ais-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

# Probe whether AIS is available by running ais-test with a single iteration
# and a zero-length duration.  An ENODEV or ENOSYS exit tells us the kernel
# does not support AIS on this machine; we emit a skip notice and exit 0 so
# the test suite is not marked failed on systems without AIS hardware.
execute_process(
  COMMAND "${AIS_TEST}" --iters 1 --duration 1 "${AIS_TARGET}"
  RESULT_VARIABLE ais_probe_result
  OUTPUT_VARIABLE ais_probe_stdout
  ERROR_VARIABLE  ais_probe_stderr
  TIMEOUT 30)

# ais-test exits 1 when an ioctl fails; check for the ENODEV hint it prints.
if(NOT ais_probe_result EQUAL 0)
  if(ais_probe_stderr MATCHES "ENODEV|ENOSYS|AIS is not initialized|ALLOC_MEMORY_OF_GPU failed|no KFD GPU")
    message(STATUS
      "ais-hardware-test: AIS not available on this machine (${ais_probe_stderr}); skipping")
    return()
  endif()
  message(FATAL_ERROR
    "ais-test probe failed unexpectedly (exit=${ais_probe_result})\n"
    "stdout:\n${ais_probe_stdout}\n"
    "stderr:\n${ais_probe_stderr}")
endif()

# AIS is present — run hsa-snoop wrapping a short ais-test workload and verify
# that AIS events appear in the JSON trace.
file(REMOVE "${TRACE_OUTPUT}")

execute_process(
  COMMAND "${HSA_SNOOP}"
    --format json
    --out "${TRACE_OUTPUT}"
    --ais-snoop
    --poll-us 1000
    -- "${AIS_TEST}" --iters 10 --duration 10 "${AIS_TARGET}"
  RESULT_VARIABLE snoop_result
  OUTPUT_VARIABLE snoop_stdout
  ERROR_VARIABLE  snoop_stderr
  TIMEOUT 60)
if(NOT snoop_result EQUAL 0)
  message(FATAL_ERROR
    "hsa-snoop ais run failed (${snoop_result})\n"
    "stdout:\n${snoop_stdout}\n"
    "stderr:\n${snoop_stderr}")
endif()

if(NOT EXISTS "${TRACE_OUTPUT}")
  message(FATAL_ERROR "hsa-snoop did not create ${TRACE_OUTPUT}")
endif()
file(READ "${TRACE_OUTPUT}" trace_json)

# Verify at least one AIS read or write event was captured.
string(REGEX MATCHALL "\"name\":\"ais_read\"" ais_read_events "${trace_json}")
string(REGEX MATCHALL "\"name\":\"ais_write\"" ais_write_events "${trace_json}")
list(LENGTH ais_read_events read_count)
list(LENGTH ais_write_events write_count)
math(EXPR ais_count "${read_count} + ${write_count}")
if(ais_count EQUAL 0)
  message(FATAL_ERROR
    "no AIS events were decoded in the trace\n${snoop_stderr}")
endif()

message(STATUS
  "AIS: decoded ${ais_count} AIS events (${read_count} read, ${write_count} write)")
