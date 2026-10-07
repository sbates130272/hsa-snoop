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

# AQL dispatch events appear as "kernel_dispatch" when the kernel name is
# resolved, or as "kernel_0x<addr>" when the process has already exited and
# the kernel object VA is no longer in /proc/maps. Both are valid captures.
string(REGEX MATCHALL "\"name\":\"kernel_dispatch\"" named_events "${trace_json}")
string(REGEX MATCHALL "\"name\":\"kernel_0x[0-9a-f]+" addr_events "${trace_json}")
list(LENGTH named_events named_count)
list(LENGTH addr_events addr_count)
math(EXPR dispatch_count "${named_count} + ${addr_count}")
if(dispatch_count EQUAL 0)
  message(FATAL_ERROR
    "no AQL dispatch events were decoded (tried kernel_dispatch and kernel_0x*)\n${snoop_stderr}")
endif()

# Barrier packets are expected in the AQL stream.
string(REGEX MATCHALL "\"name\":\"barrier_and\"" barrier_events "${trace_json}")
list(LENGTH barrier_events barrier_count)

message(STATUS
  "GFX: decoded ${dispatch_count} kernel dispatch and ${barrier_count} barrier events")
