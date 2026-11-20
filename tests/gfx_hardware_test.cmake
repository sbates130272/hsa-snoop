if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR
   NOT DEFINED TRACE_OUTPUT)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "gfx-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

file(REMOVE "${TRACE_OUTPUT}")

execute_process(
  COMMAND "${HSA_SNOOP}"
    --format json
    --out "${TRACE_OUTPUT}"
    --poll-us 1000
    -- "${GFX_TEST}" --elements 128 --iters 1 --loops 1 --batch 3 --sleep-ms 0
  RESULT_VARIABLE snoop_result
  OUTPUT_VARIABLE snoop_stdout
  ERROR_VARIABLE snoop_stderr
  TIMEOUT 420)
if(NOT snoop_result EQUAL 0)
  message(FATAL_ERROR
    "hsa-snoop gfx run failed (${snoop_result})\n"
    "stdout:\n${snoop_stdout}\n"
    "stderr:\n${snoop_stderr}")
endif()

if(NOT EXISTS "${TRACE_OUTPUT}")
  message(FATAL_ERROR "hsa-snoop did not create ${TRACE_OUTPUT}")
endif()
file(READ "${TRACE_OUTPUT}" trace_json)

# Count kernel dispatches by packet type. The event name is the kernel name
# (or kernel_0x<addr> when it could not be resolved); vendor_specific events
# are runtime-internal packets, not launches.
string(REGEX MATCHALL "\"type\":\"kernel_dispatch\"" typed_events "${trace_json}")
list(LENGTH typed_events dispatch_count)
if(dispatch_count EQUAL 0)
  message(FATAL_ERROR
    "no AQL kernel dispatch events were decoded\n${snoop_stderr}")
endif()

# Barrier packets are expected in the AQL stream.
string(REGEX MATCHALL "\"name\":\"barrier_and\"" barrier_events "${trace_json}")
list(LENGTH barrier_events barrier_count)

message(STATUS
  "GFX: decoded ${dispatch_count} kernel dispatch and ${barrier_count} barrier events")
