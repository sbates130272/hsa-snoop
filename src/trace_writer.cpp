#include "trace_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace hsasnoop {
namespace {

// Minimum rendered slice width so packets consumed within one poll are visible.
constexpr double kMinDurSec = 100e-9;

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o.push_back('\\');
            o.push_back(c);
        } else if (c == '\n')
            o += "\\n";
        else
            o.push_back(c);
    }
    return o;
}

// ---- Minimal protobuf wire encoding (no external deps) ----
struct Pb {
    std::string buf;
    void Varint(uint64_t v) {
        while (v >= 0x80) {
            buf.push_back((v & 0x7F) | 0x80);
            v >>= 7;
        }
        buf.push_back(static_cast<char>(v));
    }
    void Tag(int field, int wire) { Varint((uint64_t(field) << 3) | wire); }
    void VarintField(int field, uint64_t v) {
        Tag(field, 0);
        Varint(v);
    }
    void StringField(int field, const std::string& s) {
        Tag(field, 2);
        Varint(s.size());
        buf += s;
    }
    void MsgField(int field, const Pb& m) {
        Tag(field, 2);
        Varint(m.buf.size());
        buf += m.buf;
    }
};

// Perfetto proto field numbers (perfetto/protos/perfetto/trace/...).
namespace pf {
constexpr int kTracePacket = 1;      // Trace.packet
constexpr int kTimestamp = 8;        // TracePacket.timestamp
constexpr int kSeqId = 10;           // TracePacket.trusted_packet_sequence_id
constexpr int kTrackEvent = 11;      // TracePacket.track_event
constexpr int kSeqFlags = 13;        // TracePacket.sequence_flags
constexpr int kTrackDescriptor = 60; // TracePacket.track_descriptor

constexpr int kTdUuid = 1;       // TrackDescriptor.uuid
constexpr int kTdName = 2;       // TrackDescriptor.name
constexpr int kTdProcess = 3;    // TrackDescriptor.process
constexpr int kTdParentUuid = 5; // TrackDescriptor.parent_uuid

constexpr int kPdPid = 1;  // ProcessDescriptor.pid
constexpr int kPdName = 6; // ProcessDescriptor.process_name

constexpr int kTeType = 9;       // TrackEvent.type
constexpr int kTeTrackUuid = 11; // TrackEvent.track_uuid
constexpr int kTeName = 23;      // TrackEvent.name
constexpr int kTeDebug = 4;      // TrackEvent.debug_annotations

constexpr int kDaName = 10; // DebugAnnotation.name
constexpr int kDaStr = 6;   // DebugAnnotation.string_value
constexpr int kDaUint = 4;  // DebugAnnotation.uint_value

constexpr int kTypeSliceBegin = 1;
constexpr int kTypeSliceEnd = 2;
constexpr int kTypeInstant = 3;

constexpr uint64_t kSeq = 1;
} // namespace pf

Pb DebugStr(const std::string& name, const std::string& val) {
    Pb d;
    d.StringField(pf::kDaName, name);
    d.StringField(pf::kDaStr, val);
    return d;
}
Pb DebugUint(const std::string& name, uint64_t val) {
    Pb d;
    d.StringField(pf::kDaName, name);
    d.VarintField(pf::kDaUint, val);
    return d;
}

uint64_t NsOf(double sec) { return static_cast<uint64_t>(sec * 1e9); }
uint64_t QueueTrackUuid(uint64_t uid) { return 0x51000000ULL + uid; }
// AIS tracks: per-(pid, pcie_id) combination. Hash pcie_id into low 16 bits.
uint64_t AisTrackUuid(int pid, const std::string& pcie_id) {
    uint64_t h = std::hash<std::string>{}(pcie_id) & 0xFFFF;
    return 0xA1500000ULL | ((uint64_t)(uint32_t)pid << 16) | h;
}

std::string Dims(const uint32_t* g) {
    char b[64];
    snprintf(b, sizeof(b), "%u x %u x %u", g[0], g[1], g[2]);
    return b;
}
std::string Dims16(const uint16_t* g) {
    char b[64];
    snprintf(b, sizeof(b), "%u x %u x %u", g[0], g[1], g[2]);
    return b;
}

} // namespace

void TraceWriter::Add(const PacketRecord& rec) {
    std::lock_guard<std::mutex> lk(mu_);
    records_.push_back(rec);
}

void TraceWriter::Add(const SdmaRecord& rec) {
    std::lock_guard<std::mutex> lk(mu_);
    sdma_records_.push_back(rec);
}

void TraceWriter::Add(const AisRecord& rec) {
    std::lock_guard<std::mutex> lk(mu_);
    ais_records_.push_back(rec);
}

void TraceWriter::RegisterQueue(const QueueInfo& q) {
    std::lock_guard<std::mutex> lk(mu_);
    queues_[q.uid] = q;
}

bool TraceWriter::Write(const std::string& path) {
    return fmt_ == Format::kJson ? WriteJson(path) : WritePerfetto(path);
}

bool TraceWriter::WriteJson(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    std::ofstream f(path);
    if (!f)
        return false;
    f << "{\"traceEvents\":[\n";
    bool first = true;

    // Metadata: process + queue (thread) names.
    for (auto& kv : queues_) {
        const QueueInfo& q = kv.second;
        if (!first)
            f << ",\n";
        first = false;
        f << "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":" << q.pid
          << ",\"tid\":0,\"args\":{\"name\":\"" << JsonEscape(q.comm) << " ("
          << q.pid << ")\"}}";
        char label[128];
        snprintf(label, sizeof(label), "%s queue %lu (gpu %u)",
                 q.is_sdma() ? "sdma" : "aql", q.uid, q.gpu_id);
        f << ",\n{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":" << q.pid
          << ",\"tid\":" << q.uid << ",\"args\":{\"name\":\"" << label
          << "\"}}";
    }

    for (const auto& r : records_) {
        const QueueInfo& q =
            queues_.count(r.queue_uid) ? queues_[r.queue_uid] : QueueInfo{};
        double dur = r.complete_ts - r.submit_ts;
        if (dur < kMinDurSec)
            dur = kMinDurSec;
        if (!first)
            f << ",\n";
        first = false;
        f << "{\"ph\":\"X\",\"pid\":" << q.pid << ",\"tid\":" << r.queue_uid
          << ",\"ts\":" << (r.submit_ts * 1e6) << ",\"dur\":" << (dur * 1e6)
          << ",\"name\":\"" << JsonEscape(r.kernel_name) << "\",\"args\":{"
          << "\"type\":\"" << aql::PacketTypeName(r.type) << "\""
          << ",\"dispatch_id\":" << r.dispatch_id
          << ",\"barrier\":" << (r.barrier ? "true" : "false");
        if (r.type == aql::PacketType::KernelDispatch) {
            f << ",\"grid\":\"" << Dims(r.grid) << "\"" << ",\"workgroup\":\""
              << Dims16(r.wg) << "\"" << ",\"kernel_object\":\"0x" << std::hex
              << r.kernel_object << std::dec
              << "\",\"private_seg\":" << r.private_seg
              << ",\"group_seg\":" << r.group_seg
              << ",\"completion_signal\":\"0x" << std::hex
              << r.completion_signal << std::dec << "\"";
        }
        if (q.ring_phys)
            f << ",\"ring_phys\":\"0x" << std::hex << q.ring_phys << std::dec
              << "\"";
        f << "}}";
    }

    // SDMA copy/DMA slices on their own per-queue track.
    for (const auto& r : sdma_records_) {
        const QueueInfo& q =
            queues_.count(r.queue_uid) ? queues_[r.queue_uid] : QueueInfo{};
        double dur = r.complete_ts - r.submit_ts;
        if (dur < kMinDurSec)
            dur = kMinDurSec;
        if (!first)
            f << ",\n";
        first = false;
        f << "{\"ph\":\"X\",\"pid\":" << q.pid << ",\"tid\":" << r.queue_uid
          << ",\"ts\":" << (r.submit_ts * 1e6) << ",\"dur\":" << (dur * 1e6)
          << ",\"name\":\"" << JsonEscape(r.op_name) << "\",\"args\":{"
          << "\"opcode\":" << static_cast<unsigned>(r.opcode)
          << ",\"seq\":" << r.seq;
        if (r.is_copy()) {
            f << ",\"bytes\":" << r.bytes << ",\"direction\":\""
              << CopyDirName(r.dir) << "\"" << ",\"src\":\"0x" << std::hex
              << r.src_addr << std::dec << "\"" << ",\"dst\":\"0x" << std::hex
              << r.dst_addr << std::dec << "\"";
        }
        if (q.ring_phys)
            f << ",\"ring_phys\":\"0x" << std::hex << q.ring_phys << std::dec
              << "\"";
        f << "}}";
    }
    // AIS ioctl slices: one track per (pid, pcie_id) combination.
    // tid for JSON = use a synthetic value: hash of (pid ^ pcie string hash).
    for (const auto& r : ais_records_) {
        double dur = r.completed ? (r.complete_ts - r.submit_ts) : 0.0;
        if (dur < kMinDurSec)
            dur = kMinDurSec;
        if (!first)
            f << ",\n";
        first = false;
        // Use pid as the JSON "pid" and a synthetic tid derived from pcie_id.
        size_t tid_hash = std::hash<std::string>{}(r.pcie_id) & 0xFFFF;
        uint64_t ais_tid = 0xA15 * 0x10000ULL + tid_hash;
        std::string label = std::string("ais/") + AisOpName(r.op);
        if (!r.pcie_id.empty() && r.pcie_id != "unknown")
            label += " [" + r.pcie_id + "]";
        f << "{\"ph\":\"X\",\"pid\":" << r.pid << ",\"tid\":" << ais_tid
          << ",\"ts\":" << (r.submit_ts * 1e6) << ",\"dur\":" << (dur * 1e6)
          << ",\"name\":\"" << JsonEscape(label) << "\",\"args\":{"
          << "\"op\":\"" << AisOpName(r.op) << "\""
          << ",\"gpu_id\":" << r.gpu_id << ",\"pcie_id\":\""
          << JsonEscape(r.pcie_id) << "\"" << ",\"size_req\":" << r.size_req
          << ",\"size_copied\":" << r.size_copied << ",\"error\":" << r.error
          << ",\"seq\":" << r.seq << "}}";
    }
    f << "\n]}\n";
    return true;
}

bool TraceWriter::WritePerfetto(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    Pb trace;

    // Track descriptors: one process track per pid, one child track per queue.
    std::map<int, bool> seen_pid;
    for (auto& kv : queues_) {
        const QueueInfo& q = kv.second;
        if (!seen_pid[q.pid]) {
            seen_pid[q.pid] = true;
            Pb proc;
            proc.VarintField(pf::kPdPid, q.pid);
            proc.StringField(pf::kPdName, q.comm);
            Pb td;
            td.VarintField(pf::kTdUuid, q.pid);
            td.MsgField(pf::kTdProcess, proc);
            Pb pkt;
            pkt.MsgField(pf::kTrackDescriptor, td);
            trace.MsgField(pf::kTracePacket, pkt);
        }
        char label[128];
        snprintf(label, sizeof(label), "%s queue %lu (gpu %u)",
                 q.is_sdma() ? "sdma" : "aql", q.uid, q.gpu_id);
        Pb td;
        td.VarintField(pf::kTdUuid, QueueTrackUuid(q.uid));
        td.StringField(pf::kTdName, label);
        td.VarintField(pf::kTdParentUuid, q.pid);
        Pb pkt;
        pkt.MsgField(pf::kTrackDescriptor, td);
        trace.MsgField(pf::kTracePacket, pkt);
    }

    // Build a time-ordered list of (ts, is_begin, record*) so slices pair up.
    struct Ev {
        uint64_t ts;
        bool begin;
        const PacketRecord* r;
    };
    std::vector<Ev> evs;
    for (const auto& r : records_) {
        double end = r.complete_ts;
        if (end - r.submit_ts < kMinDurSec)
            end = r.submit_ts + kMinDurSec;
        evs.push_back({NsOf(r.submit_ts), true, &r});
        evs.push_back({NsOf(end), false, &r});
    }
    std::stable_sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) {
        if (a.ts != b.ts)
            return a.ts < b.ts;
        return !a.begin && b.begin; // end before begin at equal ts
    });

    for (const Ev& e : evs) {
        const PacketRecord& r = *e.r;
        Pb te;
        te.VarintField(pf::kTeTrackUuid, QueueTrackUuid(r.queue_uid));
        if (e.begin) {
            te.VarintField(pf::kTeType, pf::kTypeSliceBegin);
            te.StringField(pf::kTeName, r.kernel_name);
            te.MsgField(pf::kTeDebug,
                        DebugStr("type", aql::PacketTypeName(r.type)));
            te.MsgField(pf::kTeDebug, DebugUint("dispatch_id", r.dispatch_id));
            if (r.type == aql::PacketType::KernelDispatch) {
                te.MsgField(pf::kTeDebug, DebugStr("grid", Dims(r.grid)));
                te.MsgField(pf::kTeDebug, DebugStr("workgroup", Dims16(r.wg)));
                te.MsgField(pf::kTeDebug,
                            DebugUint("kernel_object", r.kernel_object));
                te.MsgField(pf::kTeDebug, DebugUint("group_seg", r.group_seg));
                te.MsgField(pf::kTeDebug,
                            DebugUint("private_seg", r.private_seg));
                te.MsgField(pf::kTeDebug, DebugUint("completion_signal",
                                                    r.completion_signal));
            }
            const QueueInfo& q = queues_[r.queue_uid];
            if (q.ring_phys)
                te.MsgField(pf::kTeDebug, DebugUint("ring_phys", q.ring_phys));
        } else {
            te.VarintField(pf::kTeType, pf::kTypeSliceEnd);
        }
        Pb pkt;
        pkt.VarintField(pf::kTimestamp, e.ts);
        pkt.MsgField(pf::kTrackEvent, te);
        pkt.VarintField(pf::kSeqId, pf::kSeq);
        trace.MsgField(pf::kTracePacket, pkt);
    }

    // SDMA copy/DMA slices, on the same per-queue tracks (keyed by uid).
    struct SEv {
        uint64_t ts;
        bool begin;
        const SdmaRecord* r;
    };
    std::vector<SEv> sevs;
    for (const auto& r : sdma_records_) {
        double end = r.complete_ts;
        if (end - r.submit_ts < kMinDurSec)
            end = r.submit_ts + kMinDurSec;
        sevs.push_back({NsOf(r.submit_ts), true, &r});
        sevs.push_back({NsOf(end), false, &r});
    }
    std::stable_sort(sevs.begin(), sevs.end(), [](const SEv& a, const SEv& b) {
        if (a.ts != b.ts)
            return a.ts < b.ts;
        return !a.begin && b.begin; // end before begin at equal ts
    });
    for (const SEv& e : sevs) {
        const SdmaRecord& r = *e.r;
        Pb te;
        te.VarintField(pf::kTeTrackUuid, QueueTrackUuid(r.queue_uid));
        if (e.begin) {
            te.VarintField(pf::kTeType, pf::kTypeSliceBegin);
            te.StringField(pf::kTeName, r.op_name);
            te.MsgField(pf::kTeDebug, DebugUint("opcode", r.opcode));
            te.MsgField(pf::kTeDebug, DebugUint("seq", r.seq));
            if (r.is_copy()) {
                te.MsgField(pf::kTeDebug, DebugUint("bytes", r.bytes));
                te.MsgField(pf::kTeDebug,
                            DebugStr("direction", CopyDirName(r.dir)));
                te.MsgField(pf::kTeDebug, DebugUint("src", r.src_addr));
                te.MsgField(pf::kTeDebug, DebugUint("dst", r.dst_addr));
            }
        } else {
            te.VarintField(pf::kTeType, pf::kTypeSliceEnd);
        }
        Pb pkt;
        pkt.VarintField(pf::kTimestamp, e.ts);
        pkt.MsgField(pf::kTrackEvent, te);
        pkt.VarintField(pf::kSeqId, pf::kSeq);
        trace.MsgField(pf::kTracePacket, pkt);
    }

    // AIS ioctl slices: per-(pid, pcie_id) track, one slice per ioctl.
    {
        // Emit track descriptors for each unique (pid, pcie_id) pair. We need
        // a process descriptor parent for each pid we haven't already seen.
        std::map<int, bool> ais_seen_pid;
        std::map<std::string, bool> ais_seen_track; // "pid:pcie_id"

        for (const auto& r : ais_records_) {
            std::string tk_key = std::to_string(r.pid) + ":" + r.pcie_id;
            if (!ais_seen_track[tk_key]) {
                ais_seen_track[tk_key] = true;

                // Process descriptor (if not emitted for this pid yet by queue
                // loop).
                if (!seen_pid[r.pid] && !ais_seen_pid[r.pid]) {
                    ais_seen_pid[r.pid] = true;
                    Pb proc;
                    proc.VarintField(pf::kPdPid, r.pid);
                    proc.StringField(pf::kPdName, r.comm);
                    Pb td;
                    td.VarintField(pf::kTdUuid, r.pid);
                    td.MsgField(pf::kTdProcess, proc);
                    Pb pkt;
                    pkt.MsgField(pf::kTrackDescriptor, td);
                    trace.MsgField(pf::kTracePacket, pkt);
                }

                // Child track: one per (pid, pcie_id).
                char label[256];
                snprintf(label, sizeof(label), "ais [gpu %u] [%s]", r.gpu_id,
                         r.pcie_id.c_str());
                uint64_t track_uuid = AisTrackUuid(r.pid, r.pcie_id);
                Pb td;
                td.VarintField(pf::kTdUuid, track_uuid);
                td.StringField(pf::kTdName, label);
                td.VarintField(pf::kTdParentUuid, r.pid);
                Pb pkt;
                pkt.MsgField(pf::kTrackDescriptor, td);
                trace.MsgField(pf::kTracePacket, pkt);
            }
        }

        // Emit time-sorted begin/end pairs for each AIS record.
        struct AEv {
            uint64_t ts;
            bool begin;
            const AisRecord* r;
        };
        std::vector<AEv> aevs;
        for (const auto& r : ais_records_) {
            double end = r.complete_ts;
            if (end - r.submit_ts < kMinDurSec)
                end = r.submit_ts + kMinDurSec;
            aevs.push_back({NsOf(r.submit_ts), true, &r});
            aevs.push_back({NsOf(end), false, &r});
        }
        std::stable_sort(aevs.begin(), aevs.end(),
                         [](const AEv& a, const AEv& b) {
                             if (a.ts != b.ts)
                                 return a.ts < b.ts;
                             return !a.begin && b.begin;
                         });

        for (const AEv& e : aevs) {
            const AisRecord& r = *e.r;
            uint64_t track_uuid = AisTrackUuid(r.pid, r.pcie_id);
            Pb te;
            te.VarintField(pf::kTeTrackUuid, track_uuid);
            if (e.begin) {
                te.VarintField(pf::kTeType, pf::kTypeSliceBegin);
                std::string name = std::string("ais/") + AisOpName(r.op);
                te.StringField(pf::kTeName, name);
                te.MsgField(pf::kTeDebug, DebugStr("op", AisOpName(r.op)));
                te.MsgField(pf::kTeDebug, DebugUint("gpu_id", r.gpu_id));
                te.MsgField(pf::kTeDebug, DebugStr("pcie_id", r.pcie_id));
                te.MsgField(pf::kTeDebug, DebugUint("size_req", r.size_req));
                te.MsgField(pf::kTeDebug,
                            DebugUint("size_copied", r.size_copied));
                if (r.is_error()) {
                    char errbuf[32];
                    snprintf(errbuf, sizeof(errbuf), "%d", r.error);
                    te.MsgField(pf::kTeDebug, DebugStr("error", errbuf));
                }
                te.MsgField(pf::kTeDebug, DebugUint("seq", r.seq));
            } else {
                te.VarintField(pf::kTeType, pf::kTypeSliceEnd);
            }
            Pb pkt;
            pkt.VarintField(pf::kTimestamp, e.ts);
            pkt.MsgField(pf::kTrackEvent, te);
            pkt.VarintField(pf::kSeqId, pf::kSeq);
            trace.MsgField(pf::kTracePacket, pkt);
        }
    }

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f.write(trace.buf.data(), trace.buf.size());
    return true;
}

} // namespace hsasnoop
