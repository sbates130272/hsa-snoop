#include "dispatch_log.h"

#include <cstring>
#include <ctime>
#include <string>

namespace hsasnoop {
namespace {

// Minimal RFC 8259 string escaping. Kernel names are C++ demangled signatures
// containing spaces, parentheses, commas and angle brackets; none of those need
// escaping, but template arguments can carry quotes and backslashes and an
// unescaped one would silently corrupt every downstream parser.
void AppendEscaped(std::string* out, const char* s) {
    out->push_back('"');
    for (const char* p = s; *p; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        switch (c) {
        case '"':
            out->append("\\\"");
            break;
        case '\\':
            out->append("\\\\");
            break;
        case '\n':
            out->append("\\n");
            break;
        case '\r':
            out->append("\\r");
            break;
        case '\t':
            out->append("\\t");
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out->append(buf);
            } else {
                out->push_back(static_cast<char>(c));
            }
        }
    }
    out->push_back('"');
}

void AppendKey(std::string* out, const char* key) {
    if (out->back() != '{')
        out->push_back(',');
    AppendEscaped(out, key);
    out->push_back(':');
}

void AppendStr(std::string* out, const char* key, const char* val) {
    AppendKey(out, key);
    AppendEscaped(out, val);
}

void AppendNum(std::string* out, const char* key, unsigned long long val) {
    AppendKey(out, key);
    out->append(std::to_string(val));
}

void AppendFixed(std::string* out, const char* key, double val, int prec) {
    AppendKey(out, key);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", prec, val);
    out->append(buf);
}

// RFC3339 with microsecond precision, which is what Loki's timestamp stage and
// Grafana's log view both expect.
void AppendTimestamp(std::string* out, double unix_sec) {
    AppendKey(out, "ts");
    time_t secs = static_cast<time_t>(unix_sec);
    long usec = static_cast<long>((unix_sec - static_cast<double>(secs)) * 1e6);
    if (usec < 0)
        usec = 0;
    if (usec > 999999)
        usec = 999999;
    struct tm tm_utc {};
    gmtime_r(&secs, &tm_utc);
    char buf[64];
    size_t n = strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    snprintf(buf + n, sizeof(buf) - n, ".%06ldZ", usec);
    AppendEscaped(out, buf);
}

double ClockSeconds(clockid_t clk) {
    struct timespec ts {};
    clock_gettime(clk, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec * 1e-9;
}

} // namespace

DispatchLog::DispatchLog(FILE* f, bool owns)
    : f_(f), owns_(owns),
      mono_to_unix_offset_(ClockSeconds(CLOCK_REALTIME) -
                           ClockSeconds(CLOCK_MONOTONIC_RAW)) {}

DispatchLog::~DispatchLog() {
    if (f_) {
        fflush(f_);
        if (owns_)
            fclose(f_);
    }
}

DispatchLog* DispatchLog::Open(const std::string& path) {
    if (path == "-")
        return new DispatchLog(stderr, false);
    FILE* f = fopen(path.c_str(), "ae");
    if (!f) {
        fprintf(stderr, "hsa-snoop: cannot open dispatch log '%s': %s\n",
                path.c_str(), strerror(errno));
        return nullptr;
    }
    // Line buffering, not the default full buffering: a log tailed by a
    // collector while hsa-snoop is still running must not sit in a 4 KB stdio
    // buffer for the several seconds a quiet period can last.
    setvbuf(f, nullptr, _IOLBF, 0);
    return new DispatchLog(f, true);
}

double DispatchLog::MonoToUnix(double mono_sec) const {
    return mono_sec + mono_to_unix_offset_;
}

void DispatchLog::WriteLine(const std::string& s) {
    std::lock_guard<std::mutex> lk(mu_);
    fwrite(s.data(), 1, s.size(), f_);
    fputc('\n', f_);
}

void DispatchLog::LogQueue(const QueueInfo& q, const char* kind) {
    std::string o = "{";
    AppendTimestamp(&o, MonoToUnix(q.create_ts));
    AppendStr(&o, "ev", "queue");
    AppendStr(&o, "kind", kind);
    AppendNum(&o, "gpu_id", q.gpu_id);
    AppendNum(&o, "pid", static_cast<unsigned long long>(q.pid));
    AppendStr(&o, "comm", q.comm.c_str());
    AppendNum(&o, "queue_uid", q.uid);
    AppendNum(&o, "qtype", q.qtype);
    AppendNum(&o, "ring_size", q.ring_size);
    // Hex, as strings: these are 64-bit addresses and JSON numbers are doubles
    // in most consumers, which silently loses the low bits above 2^53.
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%lx", q.ring_base);
    AppendStr(&o, "ring_va", buf);
    snprintf(buf, sizeof(buf), "0x%lx", q.ring_phys);
    AppendStr(&o, "ring_phys", buf);
    o.push_back('}');
    WriteLine(o);
}

void DispatchLog::LogDispatch(const PacketRecord& r, const Ctx& c) {
    std::string o = "{";
    AppendTimestamp(&o, MonoToUnix(r.submit_ts));
    AppendStr(&o, "ev", "dispatch");
    AppendNum(&o, "gpu_id", c.gpu_id);
    AppendNum(&o, "pid", static_cast<unsigned long long>(c.pid));
    AppendStr(&o, "comm", c.comm);
    AppendNum(&o, "queue_uid", r.queue_uid);
    AppendNum(&o, "dispatch_id", r.dispatch_id);
    AppendStr(&o, "kernel", r.kernel_name.c_str());
    AppendKey(&o, "grid");
    o.append("[" + std::to_string(r.grid[0]) + "," + std::to_string(r.grid[1]) +
             "," + std::to_string(r.grid[2]) + "]");
    AppendKey(&o, "wg");
    o.append("[" + std::to_string(r.wg[0]) + "," + std::to_string(r.wg[1]) +
             "," + std::to_string(r.wg[2]) + "]");
    AppendNum(&o, "lds", r.group_seg);
    AppendNum(&o, "scratch", r.private_seg);
    // Emitted only when the queue actually retired the packet. A zero would be
    // indistinguishable from a genuinely instantaneous kernel and would drag
    // every percentile panel down.
    if (r.completed && r.complete_ts > r.submit_ts)
        AppendFixed(&o, "dur_us", (r.complete_ts - r.submit_ts) * 1e6, 3);
    o.push_back('}');
    WriteLine(o);
}

void DispatchLog::LogSdma(const SdmaRecord& r, const Ctx& c) {
    std::string o = "{";
    AppendTimestamp(&o, MonoToUnix(r.submit_ts));
    AppendStr(&o, "ev", "sdma");
    AppendNum(&o, "gpu_id", c.gpu_id);
    AppendNum(&o, "pid", static_cast<unsigned long long>(c.pid));
    AppendStr(&o, "comm", c.comm);
    AppendNum(&o, "queue_uid", r.queue_uid);
    AppendNum(&o, "seq", r.seq);
    AppendStr(&o, "op", r.op_name.c_str());
    if (r.is_copy()) {
        AppendStr(&o, "dir", CopyDirName(r.dir));
        AppendNum(&o, "bytes", r.bytes);
    }
    if (r.completed && r.complete_ts > r.submit_ts)
        AppendFixed(&o, "dur_us", (r.complete_ts - r.submit_ts) * 1e6, 3);
    o.push_back('}');
    WriteLine(o);
}

} // namespace hsasnoop
