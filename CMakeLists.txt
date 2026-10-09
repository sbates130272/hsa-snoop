cmake_minimum_required(VERSION 3.21)
project(hsa-snoop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Build type (Debug|Release|RelWithDebInfo|MinSizeRel)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
      "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

add_compile_options(
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall>
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wextra>
)

find_package(Threads REQUIRED)
include(CTest)

# ---- Optional Prometheus exporter ----
option(HSA_SNOOP_PROMETHEUS "Build Prometheus exporter support" OFF)
option(HSA_SNOOP_HARDWARE_TESTS
  "Enable tests that require an AMD GPU, ROCm, and root privileges" OFF)
set(HSA_SNOOP_HARDWARE_SDMA_VERSION "AUTO" CACHE STRING
  "Expected SDMA generation for hardware tests (AUTO, 4, or 6)")
set_property(CACHE HSA_SNOOP_HARDWARE_SDMA_VERSION PROPERTY STRINGS AUTO 4 6)

if(HSA_SNOOP_HARDWARE_TESTS AND NOT BUILD_TESTING)
  message(FATAL_ERROR
    "HSA_SNOOP_HARDWARE_TESTS requires BUILD_TESTING=ON")
endif()

if(NOT HSA_SNOOP_HARDWARE_SDMA_VERSION MATCHES "^(AUTO|4|6)$")
  message(FATAL_ERROR
    "HSA_SNOOP_HARDWARE_SDMA_VERSION must be AUTO, 4, or 6")
endif()

if(HSA_SNOOP_PROMETHEUS)
  include(FetchContent)
  set(ENABLE_PUSH        OFF CACHE BOOL "" FORCE)
  set(ENABLE_COMPRESSION OFF CACHE BOOL "" FORCE)
  set(ENABLE_TESTING     OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    prometheus-cpp
    GIT_REPOSITORY https://github.com/jupp0r/prometheus-cpp.git
    GIT_TAG        v1.2.4
  )
  FetchContent_MakeAvailable(prometheus-cpp)
endif()

file(GLOB HSA_SNOOP_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(hsa-snoop ${HSA_SNOOP_SOURCES})
target_compile_features(hsa-snoop PRIVATE cxx_std_17)
target_link_libraries(hsa-snoop PRIVATE Threads::Threads)

if(BUILD_TESTING)
  add_executable(sdma-packet-test
    tests/sdma_packet_test.cpp
    src/sdma.cpp
  )
  target_include_directories(sdma-packet-test PRIVATE src)
  add_test(NAME sdma-packet-test COMMAND sdma-packet-test)

  add_executable(sdma-ring-test
    tests/sdma_ring_test.cpp
    src/aql.cpp
    src/ksym.cpp
    src/parser.cpp
    src/proc_mem.cpp
    src/sdma.cpp
  )
  target_include_directories(sdma-ring-test PRIVATE src)
  target_link_libraries(sdma-ring-test PRIVATE Threads::Threads)
  add_test(NAME sdma-ring-test COMMAND sdma-ring-test)

  # Cross-process read helpers and the kernarg footprint scan, exercised
  # against the test process's own address space. No GPU, ROCm or root needed.
  add_executable(proc-mem-test
    tests/proc_mem_test.cpp
    src/proc_mem.cpp
  )
  target_include_directories(proc-mem-test PRIVATE src)
  target_link_libraries(proc-mem-test PRIVATE Threads::Threads)
  add_test(NAME proc-mem-test COMMAND proc-mem-test)

  add_executable(aql-packet-test
    tests/aql_packet_test.cpp
    src/aql.cpp
  )
  target_include_directories(aql-packet-test PRIVATE src)
  add_test(NAME aql-packet-test COMMAND aql-packet-test)

  # Eviction regression: validates that the parser evicts a queue with unmapped
  # wptr/rptr after the failure threshold and that a healthy sibling queue
  # continues to produce records. No GPU, ROCm, or root needed.
  add_executable(parser-eviction-test
    tests/parser_eviction_test.cpp
    src/aql.cpp
    src/ksym.cpp
    src/parser.cpp
    src/proc_mem.cpp
    src/sdma.cpp
  )
  target_include_directories(parser-eviction-test PRIVATE src)
  target_link_libraries(parser-eviction-test PRIVATE Threads::Threads)
  add_test(NAME parser-eviction-test COMMAND parser-eviction-test)
endif()

if(HSA_SNOOP_PROMETHEUS)
  set(HSA_SNOOP_EXECUTABLE "${CMAKE_INSTALL_FULL_BINDIR}/hsa-snoop")
  configure_file(systemd/hsa-snoop-prometheus.service
                 "${CMAKE_CURRENT_BINARY_DIR}/hsa-snoop-prometheus.service"
                 @ONLY)
  target_compile_definitions(hsa-snoop PRIVATE HSA_SNOOP_PROMETHEUS_ENABLED)
  target_link_libraries(hsa-snoop PRIVATE prometheus-cpp::core prometheus-cpp::pull)
  message(STATUS "Prometheus exporter enabled (default port: 9488)")
endif()

install(TARGETS hsa-snoop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(HSA_SNOOP_PROMETHEUS)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/hsa-snoop-prometheus.service"
          DESTINATION lib/systemd/system)
  # Install the config template to share/hsa-snoop/; postinst copies it to
  # /etc/default/hsa-snoop on first install with auto-detected UID/GID values.
  install(FILES systemd/hsa-snoop.conf
          DESTINATION ${CMAKE_INSTALL_DATADIR}/hsa-snoop)
endif()
install(FILES grafana/hsa-snoop-dashboard.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/hsa-snoop/grafana)

# ais-test uses raw KFD ioctls only — no HIP runtime dependency.
add_executable(ais-test examples/ais-test.hip)
set_source_files_properties(examples/ais-test.hip PROPERTIES LANGUAGE CXX)
target_include_directories(ais-test PRIVATE examples)

list(APPEND CMAKE_PREFIX_PATH /opt/rocm)
find_package(hip QUIET)
if(hip_FOUND)
  message(STATUS "ROCm HIP found — building examples/")
  add_subdirectory(examples)
else()
  message(STATUS "ROCm HIP not found — skipping HIP examples (gfx-test, sdma-test)")
endif()

if(HSA_SNOOP_HARDWARE_TESTS)
  if(NOT hip_FOUND)
    message(FATAL_ERROR
      "HSA_SNOOP_HARDWARE_TESTS requires a ROCm HIP installation")
  endif()
  add_test(
    NAME sdma-hardware-test
    COMMAND ${CMAKE_COMMAND}
      -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
      -DSDMA_TEST=$<TARGET_FILE:sdma-test>
      -DTRACE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sdma-hardware-test.json
      -DEXPECTED_SDMA_VERSION=${HSA_SNOOP_HARDWARE_SDMA_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sdma_hardware_test.cmake)
  set_tests_properties(sdma-hardware-test PROPERTIES
    LABELS "hardware;sdma"
    TIMEOUT 60)

  add_test(
    NAME gfx-hardware-test
    COMMAND ${CMAKE_COMMAND}
      -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
      -DGFX_TEST=$<TARGET_FILE:gfx-test>
      -DTRACE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/gfx-hardware-test.json
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx_hardware_test.cmake)
  set_tests_properties(gfx-hardware-test PROPERTIES
    LABELS "hardware;gfx"
    TIMEOUT 480)

  # AIS target: path to a file on an NVMe-backed filesystem (or a block device).
  # When empty the test probes and skips gracefully if AIS is unavailable.
  set(HSA_SNOOP_HARDWARE_AIS_TARGET "" CACHE STRING
    "Path to the AIS test target file on an NVMe-backed filesystem")
  if(NOT HSA_SNOOP_HARDWARE_AIS_TARGET STREQUAL "")
    set(_ais_target_arg "${HSA_SNOOP_HARDWARE_AIS_TARGET}")
  else()
    # Default to the NVMe-backed file prepared by the CI NVMe setup step.
    # The probe detects AIS availability and skips gracefully if unsupported.
    set(_ais_target_arg "/mnt/nvme/ais-test.bin")
  endif()
  add_test(
    NAME ais-hardware-test
    COMMAND ${CMAKE_COMMAND}
      -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
      -DAIS_TEST=$<TARGET_FILE:ais-test>
      -DTRACE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/ais-hardware-test.json
      -DAIS_TARGET=${_ais_target_arg}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/ais_hardware_test.cmake)
  set_tests_properties(ais-hardware-test PROPERTIES
    LABELS "hardware;ais"
    TIMEOUT 120)

  # --all mode: launches two concurrent gfx-test processes and verifies that
  # hsa-snoop produces one trace file per process with kernel events in each.
  add_test(
    NAME all-mode-hardware-test
    COMMAND ${CMAKE_COMMAND}
      -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
      -DGFX_TEST=$<TARGET_FILE:gfx-test>
      -DTRACE_DIR=${CMAKE_CURRENT_BINARY_DIR}/all-mode-traces
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/all_mode_hardware_test.cmake)
  set_tests_properties(all-mode-hardware-test PROPERTIES
    LABELS "hardware;gfx"
    TIMEOUT 180)

  # SDMA stress: two streams × 4 MB, validates deeper decode than the basic
  # sdma-hardware-test (more copy_linear events, both H2D and D2D directions).
  add_test(
    NAME sdma-stress-hardware-test
    COMMAND ${CMAKE_COMMAND}
      -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
      -DSDMA_TEST=$<TARGET_FILE:sdma-test>
      -DTRACE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sdma-stress-hardware-test.json
      -DEXPECTED_SDMA_VERSION=${HSA_SNOOP_HARDWARE_SDMA_VERSION}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/sdma_stress_hardware_test.cmake)
  set_tests_properties(sdma-stress-hardware-test PROPERTIES
    LABELS "hardware;sdma"
    TIMEOUT 90)

  # Prometheus monotonicity: scrapes /metrics five times during a 30-second
  # run and asserts that hsa_kernel_launches_total never decreases.
  # Requires --prometheus support in hsa-snoop.
  if(HSA_SNOOP_PROMETHEUS)
    add_test(
      NAME prometheus-monotonicity-hardware-test
      COMMAND ${CMAKE_COMMAND}
        -DHSA_SNOOP=$<TARGET_FILE:hsa-snoop>
        -DGFX_TEST=$<TARGET_FILE:gfx-test>
        -DSCRATCH_DIR=${CMAKE_CURRENT_BINARY_DIR}/prom-monotonicity-scratch
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/prometheus_monotonicity_hardware_test.cmake)
    set_tests_properties(prometheus-monotonicity-hardware-test PROPERTIES
      LABELS "hardware;gfx;prometheus"
      TIMEOUT 120)
  endif()
endif()
