#pragma once

#ifdef HSA_SNOOP_PROMETHEUS_ENABLED

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <prometheus/registry.h>

#include "model.h"

namespace hsasnoop {

// Returns "linux", "wsl2", or "windows".
std::string DetectPlatform();

// Reads /opt/rocm/.info/version or parses rocm_version.h; returns "unknown" on
// failure.
std::string DetectRocmVersion();

// On native KFD: enumerates /sys/class/kfd/kfd/topology/nodes/<N>/gpu_id to
// match gpu_id, then reads nodes/<N>/name.
// On WSL2 (no KFD sysfs): parses `rocminfo` for the GFX target string (e.g.
// "gfx1151"). Returns "unknown" if not found.
std::string DetectGpuType(uint32_t gpu_id);

class PrometheusExporter {
  public:
    // discovery_mode: "kprobe" for native KFD, "dxg" for WSL2/librocdxg.
    explicit PrometheusExporter(uint16_t port = 9488,
                                double rate_window_sec = 10.0,
                                const std::string& discovery_mode = "kprobe");
    ~PrometheusExporter();

    // Called for each newly discovered AQL queue. Thread-safe.
    void RegisterQueue(const QueueInfo& q);

    // Called for each completed PacketRecord from RingParser. Thread-safe.
    void Add(const PacketRecord& rec);

    // Called for each completed SdmaRecord (DMA copy/op) from RingParser.
    // Thread-safe.
    void Add(const SdmaRecord& rec);

    // Called for each completed AisRecord from AisMonitor. Thread-safe.
    void Add(const AisRecord& rec);

    PrometheusExporter(const PrometheusExporter&) = delete;
    PrometheusExporter& operator=(const PrometheusExporter&) = delete;

  private:
    void RateThread();
    void UpdateRateGauges();

    struct QueueMeta {
        uint32_t gpu_id;
        int pid;
        std::string comm;
        std::string gpu_type;
    };

    struct LaunchEvent {
        std::chrono::steady_clock::time_point ts;
        uint32_t gpu_id;
    };

    // Per-GPU state for the "last kernel" info gauge.
    struct LastKernelState {
        std::string kernel_name;
        prometheus::Gauge* gauge{nullptr};
    };

    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer> exposer_;

    prometheus::Family<prometheus::Gauge>& up_family_;
    prometheus::Family<prometheus::Counter>& kernel_launches_family_;
    prometheus::Family<prometheus::Counter>& errors_family_;
    prometheus::Family<prometheus::Counter>& barrier_family_;
    prometheus::Family<prometheus::Gauge>& active_queues_family_;
    prometheus::Family<prometheus::Gauge>& launch_rate_family_;
    prometheus::Family<prometheus::Gauge>& last_launch_ts_family_;
    prometheus::Family<prometheus::Gauge>& last_kernel_info_family_;
    prometheus::Family<prometheus::Histogram>& duration_family_;
    prometheus::Family<prometheus::Gauge>& triggered_family_;
    // SDMA (DMA copy engine) telemetry.
    prometheus::Family<prometheus::Counter>& sdma_copies_family_;
    prometheus::Family<prometheus::Counter>& sdma_bytes_family_;
    prometheus::Family<prometheus::Counter>& sdma_packets_family_;
    prometheus::Family<prometheus::Gauge>& active_sdma_queues_family_;
    // Latches at 1 the first time an SDMA queue is registered; stays 0 on APU
    // nodes (e.g. Strix Halo) where all memory is shared and no SDMA engine
    // exists. Grafana panels can gate on this metric.
    prometheus::Family<prometheus::Gauge>& sdma_present_family_;
    // AIS (AMD Infinity Storage) P2P direct-storage metrics.
    // Reads (file→VRAM) and writes (VRAM→file) are separate metric families
    // rather than a label so that dashboards can select them independently.
    prometheus::Family<prometheus::Counter>& ais_rx_ops_family_;
    prometheus::Family<prometheus::Counter>& ais_tx_ops_family_;
    prometheus::Family<prometheus::Counter>& ais_rx_bytes_family_;
    prometheus::Family<prometheus::Counter>& ais_tx_bytes_family_;
    prometheus::Family<prometheus::Counter>& ais_rx_errors_family_;
    prometheus::Family<prometheus::Counter>& ais_tx_errors_family_;
    prometheus::Family<prometheus::Histogram>& ais_rx_latency_family_;
    prometheus::Family<prometheus::Histogram>& ais_tx_latency_family_;
    prometheus::Family<prometheus::Histogram>& ais_rx_io_size_family_;
    prometheus::Family<prometheus::Histogram>& ais_tx_io_size_family_;
    prometheus::Family<prometheus::Gauge>& ais_active_family_;
    // Info metric: one time-series per unique PCIe device, value=1, all labels.
    prometheus::Family<prometheus::Gauge>& ais_pcie_info_family_;

    // Guards all metadata maps and per-gpu gauge pointer caches.
    std::mutex meta_mu_;
    std::unordered_map<uint32_t, QueueMeta> queue_meta_; // uid -> meta
    std::unordered_map<std::string, prometheus::Gauge*>
        active_queue_gauges_; // gpu_id_str -> gauge
    std::unordered_map<std::string, prometheus::Gauge*>
        active_sdma_queue_gauges_; // gpu_id_str -> gauge
    std::unordered_map<std::string, prometheus::Gauge*>
        last_launch_ts_gauges_; // gpu_id_str -> gauge
    std::unordered_map<std::string, LastKernelState>
        last_kernel_state_; // gpu_id_str -> state
    std::unordered_map<std::string, prometheus::Gauge*>
        triggered_gauges_; // gpu_id_str -> gauge (latches at 1 on first
                           // dispatch)
    prometheus::Gauge* sdma_present_gauge_{
        nullptr}; // latches at 1 on first SDMA queue
    prometheus::Gauge* ais_active_gauge_{
        nullptr}; // latches at 1 on first AIS op

    // Guards the launch-event deque used for rate computation.
    std::mutex rate_mu_;
    std::deque<LaunchEvent> launch_events_;
    // Guards rate_gauges_ (written by rate thread, read in UpdateRateGauges).
    std::unordered_map<std::string, prometheus::Gauge*>
        rate_gauges_; // gpu_id_str -> gauge
    double rate_window_sec_;

    std::thread rate_thread_;
    std::atomic<bool> rate_running_{false};
};

} // namespace hsasnoop

#endif // HSA_SNOOP_PROMETHEUS_ENABLED
