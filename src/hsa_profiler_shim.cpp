// hsa_profiler_shim.cpp - HSA_TOOLS_LIB shim for hsa-snoop.
//
// Set HSA_TOOLS_LIB=/path/to/libhsa-snoop-profiler.so before launching a
// ROCm workload to get per-kernel hardware timestamps. The GPU Command
// Processor writes a dispatch's start and end ticks into the completion
// signal its AQL packet points at (amd_signal_s offsets +32 / +40), and
// hsa-snoop reads them from there. Two things have to be true for that to
// happen, and the shim arranges both:
//
//  1. Profiling is enabled on the queue. The shim intercepts
//     hsa_queue_create and hsa_amd_queue_create and calls
//     hsa_amd_profiling_set_profiler_enabled on each new compute queue.
//  2. The dispatch has a completion signal. HIP leaves it 0 on ordinary
//     kernel launches and signals at sync points with barrier packets, which
//     gives the CP nowhere to write. For queues made with hsa_queue_create
//     the shim creates an HSA intercept queue instead, and gives every
//     kernel dispatch submitted without a signal one from a small per-queue
//     pool before it reaches the hardware queue.
//
// HIP itself creates queues with the descriptor-based hsa_amd_queue_create,
// which has no intercept equivalent. The shim enables profiling on those
// queues (1) but cannot attach signals (2); run a HIP application with
// GPU_FORCE_QUEUE_PROFILING=1 as well, which makes the HIP runtime attach
// one to every kernel dispatch.
//
// Pool signals are GPU-only (no KFD event) and reused round-robin; nothing
// in the application ever waits on them. If more than kPoolSize dispatches
// are in flight on one queue a signal is reused early and the timestamps of
// the dispatches sharing it are unreliable.
//
// Usage:
//   env HSA_TOOLS_LIB=/usr/lib/libhsa-snoop-profiler.so
//       GPU_FORCE_QUEUE_PROFILING=1 ./your_hip_app
//
// HSA_SNOOP_PROFILER_NO_INTERCEPT=1 disables (2) and leaves only (1).
// HSA_SNOOP_PROFILER_DEBUG=1 prints, at unload, how many signals were
// attached and how many of the pooled signals hold timestamps.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <hsa/hsa.h>
#include <hsa/hsa_api_trace.h>
#include <hsa/hsa_ext_amd.h>
#include <mutex>
#include <vector>

namespace {

constexpr size_t kPoolSize = 1024;
constexpr size_t kPacketSize = 64;
// completion_signal is the last 8 bytes of both hsa_kernel_dispatch_packet_t
// and hsa_amd_ext_kernel_dispatch_packet_t.
constexpr size_t kCompletionSignalOffset = 56;
constexpr uint8_t kAmdFormatExtKernelDispatch = 3;
constexpr uint8_t kAmdFormatExtKernelDispatchLd = 4;

decltype(hsa_queue_create)* real_queue_create = nullptr;
#ifdef HSA_SNOOP_HAVE_AMD_QUEUE_CREATE
decltype(hsa_amd_queue_create)* real_amd_queue_create = nullptr;
#endif
decltype(hsa_amd_queue_intercept_create)* real_intercept_create = nullptr;
decltype(hsa_amd_queue_intercept_register)* real_intercept_register = nullptr;
decltype(hsa_amd_profiling_set_profiler_enabled)* real_set_profiler_enabled =
    nullptr;
decltype(hsa_amd_signal_create)* real_signal_create = nullptr;
decltype(hsa_signal_store_relaxed)* real_signal_store = nullptr;

bool intercept_enabled = true;
std::atomic<uint64_t> attached{0};
std::atomic<uint64_t> profiled_queues{0};

// One per intercepted queue. Never freed: a queue's packets can still be in
// the handler while the application destroys it, and a process creates only
// a handful of queues.
struct QueueSignals {
    std::mutex mu;
    std::vector<hsa_signal_t> pool;
    size_t next = 0;

    hsa_signal_t Take() {
        std::lock_guard<std::mutex> lk(mu);
        if (pool.size() < kPoolSize) {
            hsa_signal_t s{};
            if (real_signal_create(1, 0, nullptr, HSA_AMD_SIGNAL_AMD_GPU_ONLY,
                                   &s) != HSA_STATUS_SUCCESS)
                return hsa_signal_t{};
            pool.push_back(s);
            return s;
        }
        hsa_signal_t s = pool[next];
        next = (next + 1) % pool.size();
        real_signal_store(s, 1); // re-arm: the CP decrements it on completion
        return s;
    }
};

std::mutex registry_mu;
std::vector<QueueSignals*> registry;

bool IsKernelDispatch(const uint8_t* pkt) {
    uint16_t header;
    memcpy(&header, pkt, sizeof(header));
    const unsigned type = (header >> HSA_PACKET_HEADER_TYPE) & 0xFF;
    if (type == HSA_PACKET_TYPE_KERNEL_DISPATCH)
        return true;
    return type == HSA_PACKET_TYPE_VENDOR_SPECIFIC &&
           (pkt[2] == kAmdFormatExtKernelDispatch ||
            pkt[2] == kAmdFormatExtKernelDispatchLd);
}

// Called by the intercept queue with the packets the application submitted;
// whatever is passed to `writer` goes on the hardware queue.
void OnPackets(const void* pkts, uint64_t pkt_count,
               uint64_t /*user_pkt_index*/, void* data,
               hsa_amd_queue_intercept_packet_writer writer) {
    auto* signals = static_cast<QueueSignals*>(data);
    static thread_local std::vector<uint8_t> copy;
    copy.assign(static_cast<const uint8_t*>(pkts),
                static_cast<const uint8_t*>(pkts) + pkt_count * kPacketSize);
    for (uint64_t i = 0; i < pkt_count; ++i) {
        uint8_t* pkt = copy.data() + i * kPacketSize;
        if (!IsKernelDispatch(pkt))
            continue;
        hsa_signal_t signal;
        memcpy(&signal, pkt + kCompletionSignalOffset, sizeof(signal));
        if (signal.handle != 0)
            continue; // the application's own signal already gets the ticks
        signal = signals->Take();
        if (signal.handle == 0)
            continue;
        memcpy(pkt + kCompletionSignalOffset, &signal, sizeof(signal));
        attached.fetch_add(1, std::memory_order_relaxed);
    }
    writer(copy.data(), pkt_count);
}

hsa_status_t intercepted_queue_create(
    hsa_agent_t agent, uint32_t size, hsa_queue_type32_t type,
    void (*callback)(hsa_status_t, hsa_queue_t*, void*), void* data,
    uint32_t private_seg, uint32_t group_seg, hsa_queue_t** queue) {
    hsa_status_t st = HSA_STATUS_ERROR;
    bool intercepting = false;
    if (intercept_enabled) {
        st = real_intercept_create(agent, size, type, callback, data,
                                   private_seg, group_seg, queue);
        intercepting = st == HSA_STATUS_SUCCESS && queue && *queue;
    }
    // Fall back to an ordinary queue rather than fail the application.
    if (!intercepting)
        st = real_queue_create(agent, size, type, callback, data, private_seg,
                               group_seg, queue);
    if (st != HSA_STATUS_SUCCESS || !queue || !*queue)
        return st;

    if (intercepting) {
        auto* signals = new QueueSignals;
        if (real_intercept_register(*queue, OnPackets, signals) ==
            HSA_STATUS_SUCCESS) {
            std::lock_guard<std::mutex> lk(registry_mu);
            registry.push_back(signals);
        } else {
            delete signals;
        }
    }
    if (real_set_profiler_enabled &&
        real_set_profiler_enabled(*queue, 1) == HSA_STATUS_SUCCESS)
        profiled_queues.fetch_add(1, std::memory_order_relaxed);
    return st;
}

#ifdef HSA_SNOOP_HAVE_AMD_QUEUE_CREATE
// HIP creates its queues with this descriptor-based call. It has no
// intercept-queue equivalent, so only profiling is enabled here: a dispatch
// on such a queue gets timestamps if the application gave it a completion
// signal (HIP does under GPU_FORCE_QUEUE_PROFILING=1).
hsa_status_t intercepted_amd_queue_create(hsa_agent_t agent,
                                          hsa_amd_queue_create_desc_t* descs,
                                          uint32_t num_descs) {
    hsa_status_t st = real_amd_queue_create(agent, descs, num_descs);
    if (st != HSA_STATUS_SUCCESS || !descs || !real_set_profiler_enabled)
        return st;
    for (uint32_t i = 0; i < num_descs; ++i) {
        if (descs[i].engine_type != HSA_AMD_QUEUE_ENGINE_COMPUTE ||
            !descs[i].queue)
            continue;
        if (real_set_profiler_enabled(descs[i].queue, 1) == HSA_STATUS_SUCCESS)
            profiled_queues.fetch_add(1, std::memory_order_relaxed);
    }
    return st;
}
#endif

} // namespace

extern "C" bool OnLoad(void* table, uint64_t /*runtime_version*/,
                       uint64_t /*failed_count*/,
                       const char* const* /*failed_names*/) {
    auto* api = static_cast<HsaApiTable*>(table);
    if (!api->core_ || !api->amd_ext_)
        return false;
    real_queue_create = api->core_->hsa_queue_create_fn;
    real_signal_store = api->core_->hsa_signal_store_relaxed_fn;
    real_intercept_create = api->amd_ext_->hsa_amd_queue_intercept_create_fn;
    real_intercept_register =
        api->amd_ext_->hsa_amd_queue_intercept_register_fn;
    real_signal_create = api->amd_ext_->hsa_amd_signal_create_fn;
    real_set_profiler_enabled =
        api->amd_ext_->hsa_amd_profiling_set_profiler_enabled_fn;
    intercept_enabled = !getenv("HSA_SNOOP_PROFILER_NO_INTERCEPT") &&
                        real_intercept_create && real_intercept_register &&
                        real_signal_create && real_signal_store;
    api->core_->hsa_queue_create_fn = intercepted_queue_create;
#ifdef HSA_SNOOP_HAVE_AMD_QUEUE_CREATE
    real_amd_queue_create = api->amd_ext_->hsa_amd_queue_create_fn;
    if (real_amd_queue_create)
        api->amd_ext_->hsa_amd_queue_create_fn = intercepted_amd_queue_create;
#endif
    fprintf(stderr,
            "[hsa-snoop-profiler] AQL hardware timestamps enabled on new "
            "queues (HSA_TOOLS_LIB)%s\n",
            intercept_enabled ? ", completion signals attached to dispatches"
                              : "");
    return true;
}

extern "C" void OnUnload() {
    if (!getenv("HSA_SNOOP_PROFILER_DEBUG"))
        return;
    // A ROCr signal handle is the address of its amd_signal_s.
    uint64_t pooled = 0, stamped = 0;
    std::lock_guard<std::mutex> lk(registry_mu);
    for (QueueSignals* q : registry) {
        std::lock_guard<std::mutex> qlk(q->mu);
        for (hsa_signal_t s : q->pool) {
            uint64_t ts[2];
            memcpy(ts, reinterpret_cast<const uint8_t*>(s.handle) + 32,
                   sizeof(ts));
            ++pooled;
            if (ts[0] != 0 && ts[1] > ts[0])
                ++stamped;
        }
    }
    fprintf(stderr,
            "[hsa-snoop-profiler] profiling enabled on %lu queue(s); "
            "attached %lu signal(s) on %zu intercepted queue(s); "
            "%lu of %lu pooled signal(s) hold timestamps\n",
            static_cast<unsigned long>(profiled_queues.load()),
            static_cast<unsigned long>(attached.load()), registry.size(),
            static_cast<unsigned long>(stamped),
            static_cast<unsigned long>(pooled));
}
