# hip-config-amd.cmake always uses amdgpu-arch for detection (hardcoded ON branch),
# which returns empty on WSL2/librocdxg because it cannot enumerate PCIe devices.
# Fall back to rocm_agent_enumerator, which works on WSL2 via the librocdxg path.
if(NOT DEFINED GPU_TARGETS OR GPU_TARGETS STREQUAL "")
  find_program(ROCM_AGENT_ENUMERATOR rocm_agent_enumerator
    HINTS "${ROCM_PATH}/bin" /opt/rocm/bin)
  if(ROCM_AGENT_ENUMERATOR)
    execute_process(
      COMMAND ${ROCM_AGENT_ENUMERATOR}
      OUTPUT_VARIABLE _rae_output
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REPLACE "gfx000\n" "" _rae_output "${_rae_output}")
    string(REPLACE "\n" ";" _rae_output "${_rae_output}")
    list(REMOVE_ITEM _rae_output "gfx000")
    if(_rae_output)
      set(GPU_TARGETS "${_rae_output}" CACHE STRING
          "AMD GPU offload targets (semicolon-separated gfxNNNN list)")
      message(STATUS "GPU targets detected via rocm_agent_enumerator: ${GPU_TARGETS}")
    endif()
  endif()
endif()

if(NOT DEFINED GPU_TARGETS OR GPU_TARGETS STREQUAL "")
  set(GPU_TARGETS "gfx1100" CACHE STRING
      "AMD GPU offload targets (semicolon-separated gfxNNNN list)")
  message(WARNING
    "GPU target detection failed. Defaulting to gfx1100. "
    "Override with -DGPU_TARGETS=gfx1151 or similar.")
endif()
message(STATUS "HIP GPU targets: ${GPU_TARGETS}")

# Must be set before enable_language(HIP) so the CMake HIP driver uses the
# same architecture list as the hip::device imported target.
set(CMAKE_HIP_ARCHITECTURES ${GPU_TARGETS})
enable_language(HIP)

set_source_files_properties(gfx-test.hip PROPERTIES LANGUAGE HIP)
add_executable(gfx-test gfx-test.hip)
target_link_libraries(gfx-test PRIVATE hip::device)

set_source_files_properties(sdma-test.hip PROPERTIES LANGUAGE HIP)
add_executable(sdma-test sdma-test.hip)
target_link_libraries(sdma-test PRIVATE hip::device)

# AIS (AMD Infinity Storage) P2P direct IO test.
# Uses raw KFD ioctls only; no HIP runtime dependency.
# Built as .hip for consistency but compiled as plain C++.
set_source_files_properties(ais-test.hip PROPERTIES LANGUAGE CXX)
add_executable(ais-test ais-test.hip)
