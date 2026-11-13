// hsa_profiler_shim.cpp - HSA_TOOLS_LIB shim for hsa-snoop.
//
// Set HSA_TOOLS_LIB=/path/to/libhsa-snoop-profiler.so before launching a
// ROCm workload to enable per-kernel hardware timestamps. When this library
// is loaded by libhsa-runtime64.so it intercepts hsa_queue_create and calls
// hsa_amd_profiling_set_profiler_enabled on each new queue, instructing the
// GPU Command Processor to write start_ts/end_ts into every AQL completion
// signal. hsa-snoop reads those fields automatically after signal-based
// completion (amd_signal_s offsets +32 / +40).
//
// Usage:
//   HSA_TOOLS_LIB=/usr/lib/libhsa-snoop-profiler.so ./your_hip_app
#include <cstdio>
#include <hsa/hsa.h>
#include <hsa/hsa_api_trace.h>
#include <hsa/hsa_ext_amd.h>

namespace {

decltype(hsa_queue_create)* real_queue_create = nullptr;
decltype(hsa_amd_profiling_set_profiler_enabled)* real_set_profiler_enabled =
    nullptr;

hsa_status_t intercepted_queue_create(
    hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
    void (*callback)(hsa_status_t, hsa_queue_t*, void*), void* data,
    uint32_t private_seg, uint32_t group_seg, hsa_queue_t** queue) {
    hsa_status_t st = real_queue_create(agent, size, type, callback, data,
                                        private_seg, group_seg, queue);
    if (st == HSA_STATUS_SUCCESS && queue && *queue &&
        real_set_profiler_enabled)
        real_set_profiler_enabled(*queue, 1);
    return st;
}

} // namespace

extern "C" bool OnLoad(void* table, uint64_t /*runtime_version*/,
                       uint64_t /*failed_count*/,
                       const char* const* /*failed_names*/) {
    auto* api = static_cast<HsaApiTable*>(table);
    if (!api->core_ || !api->amd_ext_)
        return false;
    real_queue_create = api->core_->hsa_queue_create_fn;
    real_set_profiler_enabled =
        api->amd_ext_->hsa_amd_profiling_set_profiler_enabled_fn;
    api->core_->hsa_queue_create_fn = intercepted_queue_create;
    fprintf(stderr,
            "[hsa-snoop-profiler] AQL hardware timestamps enabled on new "
            "queues (HSA_TOOLS_LIB)\n");
    return true;
}

extern "C" void OnUnload() {}
