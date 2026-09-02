// dispatch_log.h - NDJSON event log of individual queue events.
//
// Why this exists alongside the Prometheus exporter: Prometheus is an
// aggregating store sampled at the scrape interval. At the ~100 dispatches per
// second a real workload produces, a 1 s scrape collapses a hundred distinct
// kernels into one counter delta, and no dashboard can recover which kernel ran
// when. The exporter is the right shape for rates, durations and totals; it is
// structurally incapable of showing a per-launch timeline.
//
// This log is the other half: one line per event, with a wall-clock timestamp,
// suitable for shipping into Loki. LogQL can then render an actual kernel
// timeline (a state-timeline keyed on the kernel name) and still derive rates,
// while the metrics path stays cheap.
//
// Deliberately NDJSON on a plain FILE* rather than a structured-logging
// dependency: hsa-snoop's only optional dependency today is prometheus-cpp, and
// this must work in the default build with the exporter compiled out.
#pragma once

#include <cstdio>
#include <mutex>
#include <string>

#include "model.h"

namespace hsasnoop {

class DispatchLog {
  public:
    // path may be "-" for stderr. Returns nullptr and prints a diagnostic if
    // the file cannot be opened.
    static DispatchLog* Open(const std::string& path);
    ~DispatchLog();

    // Queue metadata is carried per-record by the caller rather than looked up
    // here: main.cpp already owns the queue_uid -> {gpu_id, pid, comm} map, and
    // duplicating it would mean a second lock on the hot path.
    struct Ctx {
        uint32_t gpu_id = 0;
        int pid = 0;
        const char* comm = "";
    };

    void LogQueue(const QueueInfo& q, const char* kind);
    void LogDispatch(const PacketRecord& r, const Ctx& c);
    void LogSdma(const SdmaRecord& r, const Ctx& c);

    DispatchLog(const DispatchLog&) = delete;
    DispatchLog& operator=(const DispatchLog&) = delete;

  private:
    explicit DispatchLog(FILE* f, bool owns);

    // Records carry CLOCK_MONOTONIC_RAW seconds, which is meaningless to a log
    // store. Convert with an offset sampled once at construction: sampling it
    // per event would cost two clock reads on the hot path and still drift,
    // whereas a fixed offset keeps every line on one consistent axis (which is
    // what matters for a timeline) at the cost of absolute accuracy bounded by
    // the two clocks' relative drift over the run.
    double MonoToUnix(double mono_sec) const;
    void WriteLine(const std::string& s);

    FILE* f_;
    bool owns_;
    double mono_to_unix_offset_;
    std::mutex mu_;
};

} // namespace hsasnoop
