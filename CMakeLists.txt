cmake_minimum_required(VERSION 3.21)
project(hsa-snoop VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
endif()

if(HSA_SNOOP_PROMETHEUS)
  target_compile_definitions(hsa-snoop PRIVATE HSA_SNOOP_PROMETHEUS_ENABLED)
  target_link_libraries(hsa-snoop PRIVATE prometheus-cpp::core prometheus-cpp::pull)
  message(STATUS "Prometheus exporter enabled (default port: 9488)")
endif()

install(TARGETS hsa-snoop RUNTIME DESTINATION bin)
install(FILES systemd/hsa-snoop.service
              systemd/hsa-snoop-prometheus.service
        DESTINATION lib/systemd/system)
install(FILES systemd/hsa-snoop.conf
        DESTINATION etc
        RENAME hsa-snoop.conf)
install(FILES systemd/hsa-snoop.logrotate
        DESTINATION etc/logrotate.d
        RENAME hsa-snoop)
install(FILES grafana/hsa-snoop-dashboard.json
        DESTINATION share/hsa-snoop/grafana)

list(APPEND CMAKE_PREFIX_PATH /opt/rocm)
find_package(hip QUIET)
if(hip_FOUND)
  message(STATUS "ROCm HIP found — building examples/")
  add_subdirectory(examples)
else()
  message(STATUS "ROCm HIP not found — skipping examples/")
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
endif()
