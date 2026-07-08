// trace_writer.h - Buffers observed packets and writes them as either a Chrome
// JSON trace or a native Perfetto protobuf trace. Both open in ui.perfetto.dev.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "model.h"

namespace hsasnoop {

enum class Format { kPerfetto, kJson };

class TraceWriter {
  public:
    explicit TraceWriter(Format fmt) : fmt_(fmt) {}

    // Called (thread-safe) as packets complete.
    void Add(const PacketRecord& rec);

    // Register a queue so tracks are labeled even before packets arrive.
    void RegisterQueue(const QueueInfo& q);

    // Flush the buffered trace to `path`. Returns true on success.
    bool Write(const std::string& path);

    size_t count() const { return records_.size(); }

  private:
    bool WriteJson(const std::string& path);
    bool WritePerfetto(const std::string& path);

    Format fmt_;
    std::mutex mu_;
    std::vector<PacketRecord> records_;
    std::map<uint64_t, QueueInfo> queues_; // uid -> info
};

} // namespace hsasnoop
