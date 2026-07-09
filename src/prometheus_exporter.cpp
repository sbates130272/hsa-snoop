#ifdef HSA_SNOOP_PROMETHEUS_ENABLED

#include "prometheus_exporter.h"

#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>

namespace hsasnoop {

// ---------------------------------------------------------------------------
// Helper: DetectPlatform
// ---------------------------------------------------------------------------
std::string DetectPlatform() {
    std::ifstream f("/proc/version");
    if (f.is_open()) {
        std::string line;
        std::getline(f, line);
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("microsoft") != std::string::npos)
            return "wsl2";
    }
    return "linux";
}

// ---------------------------------------------------------------------------
// Helper: DetectRocmVersion
// ---------------------------------------------------------------------------
std::string DetectRocmVersion() {
    // 1. /opt/rocm/.info/version — plain-text version string
    {
        std::ifstream f("/opt/rocm/.info/version");
        if (f.is_open()) {
            std::string v;
            std::getline(f, v);
            if (!v.empty())
                return v;
        }
    }
    // 2. Parse ROCM_VERSION_MAJOR/MINOR/PATCH from rocm_version.h
    {
        std::ifstream f("/opt/rocm/include/rocm_version.h");
        if (f.is_open()) {
            int major = -1, minor = -1, patch = -1;
            std::string line;
            while (std::getline(f, line)) {
                int v = 0;
                if (sscanf(line.c_str(), "#define ROCM_VERSION_MAJOR %d", &v) ==
                    1)
                    major = v;
                else if (sscanf(line.c_str(), "#define ROCM_VERSION_MINOR %d",
                                &v) == 1)
                    minor = v;
                else if (sscanf(line.c_str(), "#define ROCM_VERSION_PATCH %d",
                                &v) == 1)
                    patch = v;
            }
            if (major >= 0 && minor >= 0 && patch >= 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%d.%d.%d", major, minor, patch);
                return buf;
            }
        }
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Helper: DetectGpuType
// ---------------------------------------------------------------------------

// Parse `rocminfo` output for the first GPU GFX target string (e.g. "gfx1151").
// Used on WSL2 where /sys/class/kfd/kfd/topology does not exist.
// Result is cached after the first successful call; rocminfo needs GPU access
// so it must be called before privileges are dropped (i.e. at construction).
static std::string DetectGpuTypeFromRocminfo() {
    static std::string cached;
    static bool done = false;
    if (done)
        return cached.empty() ? "unknown" : cached;
    done = true;

    // Set the environment variables required for the HSA runtime to find the
    // GPU in WSL2/DXG mode, using the sudo-invoking user's session values.
    const char* sudo_uid_str = getenv("SUDO_UID");
    if (sudo_uid_str) {
        char xdg[64];
        snprintf(xdg, sizeof(xdg), "/run/user/%s", sudo_uid_str);
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        setenv("HSA_ENABLE_DXG_DETECTION", "1", 1);
    }

    FILE* pipe = popen("/opt/rocm/bin/rocminfo 2>/dev/null", "r");
    if (!pipe)
        return "unknown";

    char buf[256];
    while (fgets(buf, sizeof(buf), pipe)) {
        const char* p = buf;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (strncmp(p, "Name:", 5) != 0)
            continue;
        p += 5;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (strncmp(p, "gfx", 3) == 0) {
            cached = p;
            while (!cached.empty() &&
                   (cached.back() == '\n' || cached.back() == '\r' ||
                    cached.back() == ' '))
                cached.pop_back();
            break;
        }
    }
    pclose(pipe);
    return cached.empty() ? "unknown" : cached;
}

std::string DetectGpuType(uint32_t gpu_id) {
    // Try native KFD topology first.
    const char* base = "/sys/class/kfd/kfd/topology/nodes";
    DIR* dir = opendir(base);
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] == '.')
                continue;
            std::string id_path =
                std::string(base) + "/" + ent->d_name + "/gpu_id";
            std::ifstream id_f(id_path);
            if (!id_f.is_open())
                continue;
            uint32_t node_gpu_id = 0;
            id_f >> node_gpu_id;
            if (node_gpu_id != gpu_id)
                continue;
            std::string name_path =
                std::string(base) + "/" + ent->d_name + "/name";
            std::ifstream name_f(name_path);
            if (!name_f.is_open())
                break;
            std::string name;
            std::getline(name_f, name);
            closedir(dir);
            return name.empty() ? "unknown" : name;
        }
        closedir(dir);
    }
    // KFD topology unavailable (WSL2/DXG) — fall back to rocminfo.
    return DetectGpuTypeFromRocminfo();
}

// ---------------------------------------------------------------------------
// PrometheusExporter constructor
// ---------------------------------------------------------------------------

static std::map<std::string, std::string>
MakeConstLabels(const std::string& discovery_mode) {
    char host[256] = {};
    gethostname(host, sizeof(host) - 1);
    return {
        {"host", host},
        {"platform", DetectPlatform()},
        {"rocm_version", DetectRocmVersion()},
        {"discovery_mode", discovery_mode},
    };
}

PrometheusExporter::PrometheusExporter(uint16_t port, double rate_window_sec,
                                       const std::string& discovery_mode)
    : registry_(std::make_shared<prometheus::Registry>()),
      // Metric families — registered at construction time with constant labels.
      up_family_(
          prometheus::BuildGauge()
              .Name("hsa_snoop_up")
              .Help("1 if hsa-snoop is running and attached, 0 otherwise")
              .Labels(MakeConstLabels(discovery_mode))
              .Register(*registry_)),
      kernel_launches_family_(
          prometheus::BuildCounter()
              .Name("hsa_kernel_launches_total")
              .Help("Total HSA kernel dispatch packets observed")
              .Labels(MakeConstLabels(discovery_mode))
              .Register(*registry_)),
      errors_family_(prometheus::BuildCounter()
                         .Name("hsa_errors_total")
                         .Help("Packet arrivals for unknown queues (missed "
                               "RegisterQueue races)")
                         .Labels(MakeConstLabels(discovery_mode))
                         .Register(*registry_)),
      barrier_family_(prometheus::BuildCounter()
                          .Name("hsa_barrier_packets_total")
                          .Help("HSA barrier AQL packets observed")
                          .Labels(MakeConstLabels(discovery_mode))
                          .Register(*registry_)),
      active_queues_family_(prometheus::BuildGauge()
                                .Name("hsa_active_queues")
                                .Help("Number of currently tracked AQL queues")
                                .Labels(MakeConstLabels(discovery_mode))
                                .Register(*registry_)),
      launch_rate_family_(
          prometheus::BuildGauge()
              .Name("hsa_kernel_launches_per_second")
              .Help("Rolling kernel launch rate over the configured window")
              .Labels(MakeConstLabels(discovery_mode))
              .Register(*registry_)),
      last_launch_ts_family_(
          prometheus::BuildGauge()
              .Name("hsa_last_kernel_launch_timestamp_seconds")
              .Help("Unix timestamp (wall clock) of the most recent kernel "
                    "launch on this GPU")
              .Labels(MakeConstLabels(discovery_mode))
              .Register(*registry_)),
      last_kernel_info_family_(
          prometheus::BuildGauge()
              .Name("hsa_last_kernel_launch_info")
              .Help(
                  "Most recently launched kernel name per GPU (value always 1)")
              .Labels(MakeConstLabels(discovery_mode))
              .Register(*registry_)),
      duration_family_(prometheus::BuildHistogram()
                           .Name("hsa_kernel_duration_seconds")
                           .Help("GPU kernel execution duration from enqueue "
                                 "to completion observation")
                           .Labels(MakeConstLabels(discovery_mode))
                           .Register(*registry_)),
      rate_window_sec_(rate_window_sec) {
    // Start the HTTP exposition endpoint.
    std::string addr = "0.0.0.0:" + std::to_string(port);
    exposer_ = std::make_unique<prometheus::Exposer>(addr);
    exposer_->RegisterCollectable(registry_);

    // Warm the rocminfo/KFD gpu_type cache now, while SUDO_USER/SUDO_UID are
    // still set and before any privilege drop happens. Pass 0 as a sentinel —
    // on WSL2 this triggers the rocminfo path and populates the static cache;
    // the result is reused for every subsequent RegisterQueue call.
    DetectGpuType(0);

    // Signal that the exporter is live. Set immediately so the first scrape
    // after startup always sees hsa_snoop_up=1, even before any queue arrives.
    up_family_.Add({}).Set(1.0);

    // Start the background rate-update thread.
    rate_running_ = true;
    rate_thread_ = std::thread(&PrometheusExporter::RateThread, this);
}

PrometheusExporter::~PrometheusExporter() {
    rate_running_ = false;
    if (rate_thread_.joinable())
        rate_thread_.join();
    // exposer_ dtor stops the HTTP server; registry_ dtor releases all metrics.
}

// ---------------------------------------------------------------------------
// RegisterQueue
// ---------------------------------------------------------------------------
void PrometheusExporter::RegisterQueue(const QueueInfo& q) {
    std::string gpu_str = std::to_string(q.gpu_id);
    std::string gpu_type = DetectGpuType(q.gpu_id);

    std::lock_guard<std::mutex> lk(meta_mu_);
    queue_meta_[q.uid] = {q.gpu_id, q.pid, q.comm, gpu_type};

    auto& g = active_queue_gauges_[gpu_str];
    if (!g)
        g = &active_queues_family_.Add(
            {{"gpu_id", gpu_str}, {"gpu_type", gpu_type}});
    g->Increment();
}

// ---------------------------------------------------------------------------
// Add
// ---------------------------------------------------------------------------
void PrometheusExporter::Add(const PacketRecord& rec) {
    std::lock_guard<std::mutex> lk(meta_mu_);

    auto it = queue_meta_.find(rec.queue_uid);
    if (it == queue_meta_.end()) {
        // Packet arrived before the queue was registered — count as an error.
        // We don't have a gpu_id, so use "unknown".
        errors_family_.Add({{"gpu_id", "unknown"}, {"pid", "unknown"}})
            .Increment();
        return;
    }
    const QueueMeta& meta = it->second;
    std::string gpu_str = std::to_string(meta.gpu_id);
    std::string pid_str = std::to_string(meta.pid);

    if (rec.type == aql::PacketType::KernelDispatch) {
        // hsa_kernel_launches_total
        kernel_launches_family_
            .Add({{"kernel_name", rec.kernel_name},
                  {"gpu_id", gpu_str},
                  {"gpu_type", meta.gpu_type},
                  {"pid", pid_str},
                  {"comm", meta.comm}})
            .Increment();

        // hsa_kernel_duration_seconds
        if (rec.completed && rec.complete_ts > rec.submit_ts) {
            static const prometheus::Histogram::BucketBoundaries kBuckets{
                1e-5, 1e-4, 1e-3, 1e-2, 0.1, 1.0, 10.0};
            double dur = rec.complete_ts - rec.submit_ts;
            duration_family_
                .Add({{"kernel_name", rec.kernel_name},
                      {"gpu_id", gpu_str},
                      {"gpu_type", meta.gpu_type}},
                     kBuckets)
                .Observe(dur);
        }

        // hsa_last_kernel_launch_timestamp_seconds
        {
            struct timespec now_rt {};
            clock_gettime(CLOCK_REALTIME, &now_rt);
            double now_sec =
                static_cast<double>(now_rt.tv_sec) + now_rt.tv_nsec * 1e-9;
            auto*& ts_g = last_launch_ts_gauges_[gpu_str];
            if (!ts_g)
                ts_g = &last_launch_ts_family_.Add(
                    {{"gpu_id", gpu_str}, {"gpu_type", meta.gpu_type}});
            ts_g->Set(now_sec);
        }

        // hsa_last_kernel_launch_info — replace child when kernel name changes
        {
            auto& lks = last_kernel_state_[gpu_str];
            if (lks.kernel_name != rec.kernel_name) {
                if (lks.gauge)
                    last_kernel_info_family_.Remove(lks.gauge);
                lks.gauge = &last_kernel_info_family_.Add(
                    {{"gpu_id", gpu_str},
                     {"gpu_type", meta.gpu_type},
                     {"kernel_name", rec.kernel_name}});
                lks.gauge->Set(1.0);
                lks.kernel_name = rec.kernel_name;
            }
        }

        // Append to rate deque
        {
            std::lock_guard<std::mutex> rlk(rate_mu_);
            launch_events_.push_back(
                {std::chrono::steady_clock::now(), meta.gpu_id});
        }
    }

    if (rec.barrier) {
        barrier_family_
            .Add({{"gpu_id", gpu_str},
                  {"gpu_type", meta.gpu_type},
                  {"pid", pid_str},
                  {"comm", meta.comm}})
            .Increment();
    }
}

// ---------------------------------------------------------------------------
// Rate thread
// ---------------------------------------------------------------------------
void PrometheusExporter::RateThread() {
    while (rate_running_) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (!rate_running_)
            break;
        UpdateRateGauges();
    }
}

void PrometheusExporter::UpdateRateGauges() {
    auto now = std::chrono::steady_clock::now();
    auto cutoff = now - std::chrono::duration<double>(rate_window_sec_);

    // Prune old events and count remaining per gpu_id.
    std::unordered_map<uint32_t, int> counts;
    {
        std::lock_guard<std::mutex> rlk(rate_mu_);
        while (!launch_events_.empty() && launch_events_.front().ts < cutoff) {
            launch_events_.pop_front();
        }
        for (const auto& ev : launch_events_)
            counts[ev.gpu_id]++;
    }

    // Zero all existing rate gauges, then set non-zero values.
    std::lock_guard<std::mutex> lk(meta_mu_);
    for (auto& [gpu_str, g] : rate_gauges_)
        g->Set(0.0);

    for (const auto& [gpu_id, count] : counts) {
        std::string gpu_str = std::to_string(gpu_id);
        auto*& g = rate_gauges_[gpu_str];
        if (!g) {
            std::string gpu_type = DetectGpuType(gpu_id);
            g = &launch_rate_family_.Add(
                {{"gpu_id", gpu_str}, {"gpu_type", gpu_type}});
        }
        g->Set(static_cast<double>(count) / rate_window_sec_);
    }
}

} // namespace hsasnoop

#endif // HSA_SNOOP_PROMETHEUS_ENABLED
