#include "parser.h"

#include <time.h>
#include <unistd.h>

#include <cstring>

#include "proc_mem.h"

namespace hsasnoop {
namespace {

double MonoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

const bool kDebug = getenv("HSA_SNOOP_DEBUG") != nullptr;

} // namespace

RingParser::RingParser(Sink sink, int poll_us)
    : sink_(std::move(sink)), poll_us_(poll_us) {
    running_ = true;
    thread_ = std::thread(&RingParser::Run, this);
}

RingParser::~RingParser() { Stop(); }

void RingParser::Stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

void RingParser::AddQueue(const QueueInfo& q) {
    auto qs = std::make_unique<QueueState>();
    qs->info = q;
    qs->ksym = std::make_unique<KernelSymbolResolver>(q.pid);
    std::lock_guard<std::mutex> lk(mu_);
    queues_.push_back(std::move(qs));
}

// Read and decode the AQL packet at ring slot for `id`. Returns false if the
// packet is not yet ready (producer reserved the slot but has not published the
// header) or the read failed.
bool RingParser::DecodeSlot(QueueState* qs, uint64_t id, PacketRecord* rec) {
    const QueueInfo& q = qs->info;
    uint32_t slots = q.num_slots();
    if (!slots)
        return false;
    uint64_t addr = q.ring_base + (id % slots) * aql::kPacketSize;

    uint8_t raw[aql::kPacketSize];
    if (!ReadProcMem(q.pid, addr, raw, sizeof(raw))) {
        if (kDebug)
            fprintf(stderr, "[decode q%lu id%lu] read failed @0x%lx\n", q.uid,
                    id, addr);
        return false;
    }

    uint16_t header;
    memcpy(&header, raw, sizeof(header));
    aql::PacketType type = aql::HeaderType(header);

    // Producer publishes the header (with release) last. Treat
    // not-yet-published slots as pending so we don't decode a half-written
    // packet.
    if (type == aql::PacketType::Invalid ||
        type == aql::PacketType::VendorSpecific) {
        if (kDebug)
            fprintf(stderr, "[decode q%lu id%lu] not-ready header=0x%x\n",
                    q.uid, id, header);
        return false;
    }

    rec->queue_uid = q.uid;
    rec->dispatch_id = id;
    rec->type = type;
    rec->barrier = aql::HeaderBarrier(header);

    if (type == aql::PacketType::KernelDispatch) {
        aql::KernelDispatchPacket p;
        memcpy(&p, raw, sizeof(p));
        rec->kernel_object = p.kernel_object;
        rec->grid[0] = p.grid_size_x;
        rec->grid[1] = p.grid_size_y;
        rec->grid[2] = p.grid_size_z;
        rec->wg[0] = p.workgroup_size_x;
        rec->wg[1] = p.workgroup_size_y;
        rec->wg[2] = p.workgroup_size_z;
        rec->private_seg = p.private_segment_size;
        rec->group_seg = p.group_segment_size;
        rec->completion_signal = p.completion_signal;
        rec->kernarg_address = p.kernarg_address;
        rec->kernel_name = qs->ksym->Resolve(p.kernel_object);
    } else {
        rec->kernel_name = aql::PacketTypeName(type);
    }
    return true;
}

void RingParser::PollQueue(QueueState* qs, double now) {
    const QueueInfo& q = qs->info;
    uint64_t wptr = 0, rptr = 0;
    if (!ReadU64(q.pid, q.wptr_addr, &wptr))
        return;
    if (!ReadU64(q.pid, q.rptr_addr, &rptr))
        return;

    if (!qs->primed) {
        // Start from the current head so we only report new activity.
        qs->next_id = wptr;
        qs->last_rptr = rptr;
        qs->primed = true;
        return;
    }

    if (kDebug && (wptr != qs->next_id || rptr != qs->last_rptr)) {
        fprintf(stderr,
                "[poll q%lu] wptr=%lu rptr=%lu next_id=%lu inflight=%zu\n",
                q.uid, wptr, rptr, qs->next_id, qs->inflight.size());
    }

    // Guard against decoding slots the producer has already wrapped over.
    uint64_t slots = q.num_slots();
    if (wptr > qs->next_id + slots) {
        qs->dropped += (wptr - qs->next_id) - slots;
        qs->next_id = wptr - slots;
    }

    // Decode newly enqueued packets in order. The producer bumps
    // write_dispatch_id when it *reserves* a slot, then publishes the header
    // (type) last, so a slot with id < wptr may momentarily read as INVALID.
    // Distinguish two cases:
    //   * id >= rptr : still pending publish -> stop and retry next poll.
    //   * id <  rptr : already consumed and we missed the publish window
    //                  (kernel finished between polls) -> count as dropped,
    //                  skip.
    while (qs->next_id < wptr) {
        PacketRecord rec;
        if (DecodeSlot(qs, qs->next_id, &rec)) {
            rec.submit_ts = now;
            qs->inflight.emplace(qs->next_id, std::move(rec));
            qs->next_id++;
        } else if (qs->next_id < rptr) {
            qs->dropped++;
            qs->next_id++;
        } else {
            break; // genuinely pending publication
        }
    }

    // Mark packets as queue-consumed once read_dispatch_id advances past them.
    // This is not the same as observing the packet completion signal.
    while (!qs->inflight.empty()) {
        auto it = qs->inflight.begin();
        if (it->first >= rptr)
            break;
        PacketRecord& r = it->second;
        r.complete_ts = now;
        r.completed = true;
        if (sink_)
            sink_(r);
        qs->inflight.erase(it);
    }
    qs->last_rptr = rptr;
}

void RingParser::Run() {
    while (running_) {
        double now = MonoNow();
        std::vector<QueueState*> snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& q : queues_)
                snapshot.push_back(q.get());
        }
        for (QueueState* qs : snapshot) {
            if (!ProcAlive(qs->info.pid))
                continue;
            PollQueue(qs, now);
        }
        usleep(poll_us_);
    }
}

} // namespace hsasnoop
