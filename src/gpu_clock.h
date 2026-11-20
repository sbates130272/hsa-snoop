// gpu_clock.h - The rate of the GPU clock that timestamps dispatches.
//
// With queue profiling on, the command processor writes a start and an end
// tick into each dispatch's completion signal. The ticks are in the GPU's own
// counter, whose rate differs between parts, so they need a rate before they
// can be reported in seconds.
#pragma once
#include <cstdint>

namespace hsasnoop {

// Measures the GPU counter's rate in ticks per second by sampling it twice
// against the CPU's raw monotonic clock through KFD. Blocks for about 200 ms.
// Returns false when KFD is not available or reports no GPU counter for
// gpu_id, as an emulated GPU may.
bool MeasureGpuClockHz(uint32_t gpu_id, double* hz);

} // namespace hsasnoop
