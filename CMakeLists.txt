cmake_minimum_required(VERSION 3.21)
project(hsa-snoop LANGUAGES CXX)

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

# ---- Optional Prometheus exporter ----
option(HSA_SNOOP_PROMETHEUS "Build Prometheus exporter support" OFF)

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

list(APPEND CMAKE_PREFIX_PATH /opt/rocm)
find_package(hip QUIET)
if(hip_FOUND)
  message(STATUS "ROCm HIP found — building examples/")
  add_subdirectory(examples)
else()
  message(STATUS "ROCm HIP not found — skipping examples/")
endif()
