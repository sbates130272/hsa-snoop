if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR NOT DEFINED TRACE_DIR)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()
if(NOT DEFINED REQUIRE_GPUS)
  set(REQUIRE_GPUS 0)
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "two-gpu-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

# KFD lists a CPU-only node with gpu_id 0; every other node is a GPU.
file(GLOB _nodes "/sys/class/kfd/kfd/topology/nodes/*/gpu_id")
set(kfd_gpu_ids "")
foreach(_n IN LISTS _nodes)
  file(READ "${_n}" _id)
  string(STRIP "${_id}" _id)
  if(NOT _id STREQUAL "0" AND NOT _id STREQUAL "")
    list(APPEND kfd_gpu_ids "${_id}")
  endif()
endforeach()
list(LENGTH kfd_gpu_ids kfd_gpu_count)
if(kfd_gpu_count LESS 2)
  if(REQUIRE_GPUS GREATER 1)
    message(FATAL_ERROR
      "two-gpu-hardware-test: ${REQUIRE_GPUS} GPUs required, KFD has ${kfd_gpu_count}")
  endif()
  message(STATUS "SKIP: two-gpu-hardware-test needs two GPUs, KFD has ${kfd_gpu_count}")
  return()
endif()

file(MAKE_DIRECTORY "${TRACE_DIR}")
file(GLOB _old_traces "${TRACE_DIR}/*.json")
foreach(_f IN LISTS _old_traces)
  file(REMOVE "${_f}")
endforeach()

set(SNOOP_LOG "${TRACE_DIR}/snoop.log")
set(ORCHESTRATE_SCRIPT "${TRACE_DIR}/orchestrate.sh")

# One workload per GPU at the same time, with different launch counts, so a
# launch credited to the wrong GPU shows up in the per-GPU totals:
#   device 0: 2 loops x 3 batches x 3 kernels = 18 launches
#   device 1: 5 loops x 3 batches x 3 kernels = 45 launches
set(LOW_LAUNCHES 18)
set(HIGH_LAUNCHES 45)
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
set -e
LOG='${SNOOP_LOG}'

rm -f \"\$LOG\"
'${HSA_SNOOP}' --all --format json --out-dir '${TRACE_DIR}' \
  --poll-us 1000 > \"\$LOG\" 2>&1 &
SNOOP_PID=\$!

n=0
while [ \"\$n\" -lt 30 ]; do
  sleep 0.5
  grep -q 'discovery armed' \"\$LOG\" 2>/dev/null && break
  n=\$((n + 1))
done
if ! grep -q 'discovery armed' \"\$LOG\" 2>/dev/null; then
  echo 'ERROR: hsa-snoop did not arm in time' >&2
  kill \$SNOOP_PID 2>/dev/null || true
  exit 1
fi

HIP_VISIBLE_DEVICES=0 '${GFX_TEST}' --elements 128 --iters 1 --loops 2 --batch 3 --sleep-ms 200 &
P1=\$!
HIP_VISIBLE_DEVICES=1 '${GFX_TEST}' --elements 128 --iters 1 --loops 5 --batch 3 --sleep-ms 200 &
P2=\$!
wait \$P1
wait \$P2

sleep 3
kill \$SNOOP_PID 2>/dev/null || true
wait \$SNOOP_PID 2>/dev/null || true
")

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE orch_result
  OUTPUT_VARIABLE orch_stdout
  ERROR_VARIABLE orch_stderr
  TIMEOUT 150)

if(EXISTS "${SNOOP_LOG}")
  file(READ "${SNOOP_LOG}" snoop_log)
else()
  set(snoop_log "(no log)")
endif()
if(NOT orch_result EQUAL 0)
  message(FATAL_ERROR
    "orchestration script failed (${orch_result})\n"
    "stderr:\n${orch_stderr}\nhsa-snoop log:\n${snoop_log}")
endif()

# Each trace file is one queue; its thread name carries the GPU the queue
# was created on.
file(GLOB trace_files "${TRACE_DIR}/*.json")
set(seen_gpu_ids "")
foreach(trace_file IN LISTS trace_files)
  file(READ "${trace_file}" tj)
  string(REGEX MATCH "aql queue [0-9]+ \\(gpu ([0-9]+)\\)" _m "${tj}")
  if(NOT _m)
    continue()
  endif()
  set(_gpu "${CMAKE_MATCH_1}")
  string(REGEX MATCHALL "\"type\":\"kernel_dispatch\"" typed "${tj}")
  list(LENGTH typed kevt)
  if(kevt EQUAL 0)
    continue()
  endif()
  if(NOT DEFINED launches_${_gpu})
    set(launches_${_gpu} 0)
    list(APPEND seen_gpu_ids "${_gpu}")
  endif()
  math(EXPR launches_${_gpu} "${launches_${_gpu}} + ${kevt}")
endforeach()

list(LENGTH seen_gpu_ids seen_count)
if(NOT seen_count EQUAL 2)
  message(FATAL_ERROR
    "two-gpu-hardware-test: launches seen on ${seen_count} GPU(s) "
    "(${seen_gpu_ids}), expected 2\nhsa-snoop log:\n${snoop_log}")
endif()

set(summary "")
foreach(_gpu IN LISTS seen_gpu_ids)
  if(NOT _gpu IN_LIST kfd_gpu_ids)
    message(FATAL_ERROR
      "two-gpu-hardware-test: gpu ${_gpu} in the traces is not a KFD gpu_id "
      "(${kfd_gpu_ids})\nhsa-snoop log:\n${snoop_log}")
  endif()
  string(APPEND summary " gpu ${_gpu}: ${launches_${_gpu}} launch(es);")
endforeach()

list(GET seen_gpu_ids 0 _a)
list(GET seen_gpu_ids 1 _b)
if(launches_${_a} LESS launches_${_b})
  set(low ${launches_${_a}})
  set(high ${launches_${_b}})
else()
  set(low ${launches_${_b}})
  set(high ${launches_${_a}})
endif()
# The runtime adds a few launches of its own per process, never as many as
# the difference between the two workloads.
math(EXPR low_max "${LOW_LAUNCHES} + 12")
math(EXPR high_max "${HIGH_LAUNCHES} + 12")
if(low LESS LOW_LAUNCHES OR low GREATER low_max OR
   high LESS HIGH_LAUNCHES OR high GREATER high_max)
  message(FATAL_ERROR
    "two-gpu-hardware-test: per-GPU launches are wrong:${summary} expected "
    "${LOW_LAUNCHES}..${low_max} on one GPU and ${HIGH_LAUNCHES}..${high_max} "
    "on the other\nhsa-snoop log:\n${snoop_log}")
endif()

message(STATUS "two-gpu-hardware-test:${summary}")
