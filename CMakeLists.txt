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

file(GLOB HSA_SNOOP_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
add_executable(hsa-snoop ${HSA_SNOOP_SOURCES})
target_compile_features(hsa-snoop PRIVATE cxx_std_17)
target_link_libraries(hsa-snoop PRIVATE Threads::Threads)

list(APPEND CMAKE_PREFIX_PATH /opt/rocm)
find_package(hip QUIET)
if(hip_FOUND)
  message(STATUS "ROCm HIP found — building examples/")
  add_subdirectory(examples)
else()
  message(STATUS "ROCm HIP not found — skipping examples/")
endif()
