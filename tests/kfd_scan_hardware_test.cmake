if(NOT DEFINED HSA_SNOOP OR NOT DEFINED GFX_TEST OR NOT DEFINED TRACE_DIR)
  message(FATAL_ERROR "hardware test paths were not supplied")
endif()

execute_process(
  COMMAND id -u
  OUTPUT_VARIABLE effective_uid
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE id_result)
if(NOT id_result EQUAL 0 OR NOT effective_uid STREQUAL "0")
  message(FATAL_ERROR
    "kfd-scan-hardware-test must run as root; use sudo ctest --test-dir <build> -L hardware")
endif()

# The scan is a KFD ioctl; WSL2/librocdxg has no /dev/kfd to scan.
if(NOT EXISTS "/dev/kfd")
  message(STATUS "SKIP: no /dev/kfd on this system")
  return()
endif()

file(MAKE_DIRECTORY "${TRACE_DIR}")
set(TRACE_OUTPUT "${TRACE_DIR}/trace.json")
set(SNOOP_LOG "${TRACE_DIR}/snoop.log")
set(GFX_LOG "${TRACE_DIR}/gfx-test.log")
set(ORCHESTRATE_SCRIPT "${TRACE_DIR}/orchestrate.sh")
set(TRACER_FILE "${TRACE_DIR}/tracer-pid.txt")
set(EXIT_STATE_FILE "${TRACE_DIR}/exit-state.txt")
file(REMOVE "${TRACE_OUTPUT}" "${SNOOP_LOG}" "${GFX_LOG}" "${TRACER_FILE}"
  "${EXIT_STATE_FILE}")

# Every other hardware test starts hsa-snoop first, so each queue it sees
# arrives through the kprobe. This one covers the opposite order: the
# workload is already running, with its queues created, when hsa-snoop
# attaches. Those queues can only come from the KFD debug-trap startup scan.
#
#  1. Start gfx-test looping forever.
#  2. Wait until it has launched kernels, so its queues exist.
#  3. Attach hsa-snoop with --pid for a fixed duration.
#  4. Check the scan has detached from gfx-test again.
#  5. Stop gfx-test while hsa-snoop is still running.
file(WRITE "${ORCHESTRATE_SCRIPT}"
"#!/bin/sh
\"${GFX_TEST}\" --elements 128 --iters 1 --loops 0 --batch 3 --sleep-ms 200 \\
  > \"${GFX_LOG}\" 2>&1 &
GFX_PID=\$!

n=0
while [ \"\$n\" -lt 120 ]; do
  grep -q 'total_launches' \"${GFX_LOG}\" 2>/dev/null && break
  sleep 0.5
  n=\$((n + 1))
done
if ! grep -q 'total_launches' \"${GFX_LOG}\" 2>/dev/null; then
  echo 'ERROR: gfx-test launched no kernels in 60 s' >&2
  cat \"${GFX_LOG}\" >&2
  kill \$GFX_PID 2>/dev/null
  exit 1
fi

\"${HSA_SNOOP}\" --pid \$GFX_PID --format json --out \"${TRACE_OUTPUT}\" \\
  --poll-us 1000 --duration 12 > \"${SNOOP_LOG}\" 2>&1 &
SNOOP_PID=\$!

n=0
while [ \"\$n\" -lt 20 ]; do
  grep -q 'kfd-scan:' \"${SNOOP_LOG}\" 2>/dev/null && break
  sleep 0.5
  n=\$((n + 1))
done

# The scan attaches with ptrace. It must have let go again: a workload left
# attached takes no signals and cannot be reaped while hsa-snoop runs.
sed -n 's/^TracerPid:[[:space:]]*//p' /proc/\$GFX_PID/status \\
  > \"${TRACER_FILE}\" 2>/dev/null

# Stop the workload while hsa-snoop is still running, once there has been
# time to decode some dispatches. It has to die, not sit in a ptrace-stop.
sleep 5
kill \$GFX_PID 2>/dev/null
n=0
state=
while [ \"\$n\" -lt 50 ]; do
  state=\$(sed -n 's/^State:[[:space:]]*\\(.\\).*/\\1/p' /proc/\$GFX_PID/status 2>/dev/null)
  [ -z \"\$state\" ] || [ \"\$state\" = Z ] && break
  sleep 0.1
  n=\$((n + 1))
done
echo \"\${state:-gone}\" > \"${EXIT_STATE_FILE}\"
[ -z \"\$state\" ] || [ \"\$state\" = Z ] || kill -KILL \$GFX_PID 2>/dev/null

wait \$SNOOP_PID
rc=\$?
wait \$GFX_PID 2>/dev/null
exit \$rc
")

execute_process(
  COMMAND sh "${ORCHESTRATE_SCRIPT}"
  RESULT_VARIABLE orch_result
  ERROR_VARIABLE orch_stderr
  TIMEOUT 100)

if(EXISTS "${SNOOP_LOG}")
  file(READ "${SNOOP_LOG}" snoop_log)
else()
  set(snoop_log "(no log)")
endif()

if(NOT orch_result EQUAL 0)
  message(FATAL_ERROR
    "orchestration script failed (${orch_result})\n"
    "stderr:\n${orch_stderr}\n"
    "hsa-snoop log:\n${snoop_log}")
endif()

# The scan must have snapshotted the process and found its queues.
if(NOT snoop_log MATCHES "kfd-scan: [1-9][0-9]* process\\(es\\), ([1-9][0-9]*) queue\\(s\\)")
  message(FATAL_ERROR
    "the KFD startup scan found no pre-existing queues\n"
    "hsa-snoop log:\n${snoop_log}")
endif()
set(scan_queues "${CMAKE_MATCH_1}")

# The scan must not leave the workload ptrace-attached.
set(tracer_pid "(not recorded)")
if(EXISTS "${TRACER_FILE}")
  file(READ "${TRACER_FILE}" tracer_pid)
  string(STRIP "${tracer_pid}" tracer_pid)
endif()
if(NOT tracer_pid STREQUAL "0")
  message(FATAL_ERROR
    "gfx-test was still ptrace-attached after the startup scan "
    "(TracerPid: ${tracer_pid})\n"
    "hsa-snoop log:\n${snoop_log}")
endif()

# And the workload must be able to exit while hsa-snoop is running.
set(exit_state "(not recorded)")
if(EXISTS "${EXIT_STATE_FILE}")
  file(READ "${EXIT_STATE_FILE}" exit_state)
  string(STRIP "${exit_state}" exit_state)
endif()
if(NOT exit_state MATCHES "^(Z|gone)$")
  message(FATAL_ERROR
    "gfx-test did not exit within 5 s of SIGTERM while hsa-snoop was running "
    "(process state: ${exit_state})\n"
    "hsa-snoop log:\n${snoop_log}")
endif()

# And those queues must be live: dispatches enqueued after the attach decode.
if(NOT EXISTS "${TRACE_OUTPUT}")
  message(FATAL_ERROR
    "hsa-snoop did not create ${TRACE_OUTPUT}\nhsa-snoop log:\n${snoop_log}")
endif()
file(READ "${TRACE_OUTPUT}" trace_json)
string(REGEX MATCHALL "\"type\":\"kernel_dispatch\"" typed_events "${trace_json}")
list(LENGTH typed_events dispatch_count)
if(dispatch_count EQUAL 0)
  message(FATAL_ERROR
    "no kernel dispatches decoded on the ${scan_queues} scanned queue(s)\n"
    "hsa-snoop log:\n${snoop_log}")
endif()

message(STATUS
  "kfd-scan-hardware-test: ${scan_queues} pre-existing queue(s), "
  "${dispatch_count} kernel dispatch event(s)")
