#include "parser.h"

#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "proc_mem.h"
#include "sdma.h"

namespace hsasnoop {
namespace {

double MonoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

const bool kDebug = getenv("HSA_SNOOP_DEBUG") != nullptr;

// Calibration aid: when set, dump raw SDMA ring pointer values and the head of
// the ring buffer as hex dwords whenever the write pointer advances. Used once,
// on new hardware, to confirm the pointer unit (dword vs byte) and the copy
// packet layout, then disabled. Gated separately from HSA_SNOOP_DEBUG because
// it is very verbose.
const bool kSdmaDump = getenv("HSA_SNOOP_SDMA_DUMP") != nullptr;

// SDMA read/write pointers (the u64s at rptr_addr/wptr_addr) are, on the CDNA
// KFD path, expressed in dwords and advance monotonically; the ring wraps at
// ring_size. If a driver revision expresses them in bytes instead, set this to
// false. This is the single most important thing to confirm during on-hardware
// calibration -- run with HSA_SNOOP_DEBUG=1 and check that packet strides line
// up with the wptr deltas in the debug dump.
constexpr bool kSdmaPtrIsDwords = true;

// Largest SDMA-packet dword window we read up front to decode header + length
// and (for linear copies) the src/dst addresses. Big WRITE payloads exceed this
// but their length is derived from the count dword within the window, so we can
// still stride past them correctly.
constexpr uint32_t kSdmaWindow = 16;

// Bound the direction-classification page cache so a long-lived --all daemon
// cannot grow it without limit.
constexpr size_t kPageCacheMax = 1u << 16;

} // namespace

RingParser::RingParser(Sink sink, SdmaSink sdma_sink, int poll_us)
    : sink_(std::move(sink)), sdma_sink_(std::move(sdma_sink)),
      poll_us_(poll_us) {
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
    // Kernel-name resolution only applies to AQL dispatch packets; SDMA copies
    // carry no kernel object, so skip the (process-scanning) resolver for them.
    if (q.is_aql())
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
            if (qs->info.is_sdma())
                PollSdmaQueue(qs, now);
            else
                PollQueue(qs, now);
        }
        usleep(poll_us_);
    }
}

// ---------------------------------------------------------------------------
// SDMA path
// ---------------------------------------------------------------------------

// Read n dwords from the ring starting at dword offset start_dw (modulo the
// ring size in dwords), transparently handling a wrap across the ring end.
bool RingParser::ReadRingDwords(QueueState* qs, uint64_t start_dw, uint32_t n,
                                uint32_t* out) {
    const QueueInfo& q = qs->info;
    uint64_t ring_dw = q.num_dwords();
    if (!ring_dw)
        return false;
    uint64_t off = start_dw % ring_dw;
    uint64_t first = std::min<uint64_t>(n, ring_dw - off);
    if (!ReadProcMem(q.pid, q.ring_base + off * 4, out, first * 4))
        return false;
    if (first < n)
        return ReadProcMem(q.pid, q.ring_base, out + first, (n - first) * 4);
    return true;
}

// A user VA is host-backed when pagemap resolves it to a physical frame; a
// device (VRAM) page has no PFN and reads back 0. Cached per page.
bool RingParser::IsHostVA(QueueState* qs, uint64_t va) {
    uint64_t page = va & ~0xFFFULL;
    auto it = qs->page_host.find(page);
    if (it != qs->page_host.end())
        return it->second;
    bool host = VirtToPhys(qs->info.pid, va) != 0;
    if (qs->page_host.size() >= kPageCacheMax)
        qs->page_host.clear();
    qs->page_host[page] = host;
    return host;
}

CopyDir RingParser::ClassifyDir(QueueState* qs, uint64_t src, uint64_t dst) {
    bool src_host = IsHostVA(qs, src);
    bool dst_host = IsHostVA(qs, dst);
    if (src_host && !dst_host)
        return CopyDir::HostToDevice;
    if (!src_host && dst_host)
        return CopyDir::DeviceToHost;
    if (!src_host && !dst_host)
        return CopyDir::DeviceToDevice;
    return CopyDir::HostToHost;
}

void RingParser::PollSdmaQueue(QueueState* qs, double now) {
    const QueueInfo& q = qs->info;
    uint64_t wptr_raw = 0, rptr_raw = 0;
    if (!ReadU64(q.pid, q.wptr_addr, &wptr_raw) ||
        !ReadU64(q.pid, q.rptr_addr, &rptr_raw)) {
        // On CDNA/gfx942 the SDMA read/write pointers live in a doorbell-style
        // mapping that process_vm_readv cannot reach (EFAULT), even though the
        // ring buffer itself is readable. When that happens the pointers must
        // be sourced from the KFD debugfs queue descriptors (mqds/hqds) instead
        // -- see the SDMA notes in the README. Warn once so this degradation is
        // visible rather than a silent no-op.
        static int warn_budget = 4;
        if (warn_budget > 0) {
            --warn_budget;
            fprintf(stderr,
                    "[sdma q%lu] read/write pointers unreadable via "
                    "process_vm_readv (pid=%d wptr=0x%lx); SDMA progress "
                    "tracking unavailable on this platform\n",
                    q.uid, q.pid, q.wptr_addr);
        }
        return;
    }

    uint64_t ring_dw = q.num_dwords();
    if (!ring_dw)
        return;

    // Normalise both pointers to dword counts.
    uint64_t wptr = kSdmaPtrIsDwords ? wptr_raw : wptr_raw / 4;
    uint64_t rptr = kSdmaPtrIsDwords ? rptr_raw : rptr_raw / 4;

    // Calibration dump: raw pointer magnitudes + head of the ring, so the
    // pointer unit and packet layout can be read off against a known workload.
    // Bounded so it cannot flood a long run.
    static int dump_budget = 16;
    if (kSdmaDump && dump_budget > 0 && wptr_raw != rptr_raw) {
        --dump_budget;
        fprintf(stderr,
                "[sdma-dump q%lu] pid=%d wptr_raw=%lu rptr_raw=%lu "
                "ring_size=%uB ring_base=0x%lx\n",
                q.uid, q.pid, wptr_raw, rptr_raw, q.ring_size, q.ring_base);
        uint32_t ndump = static_cast<uint32_t>(std::min<uint64_t>(ring_dw, 64));
        uint32_t buf[64];
        if (ReadRingDwords(qs, 0, ndump, buf)) {
            for (uint32_t i = 0; i < ndump; i += 8) {
                fprintf(stderr, "  dw%02u:", i);
                for (uint32_t j = i; j < i + 8 && j < ndump; ++j)
                    fprintf(stderr, " %08x", buf[j]);
                fprintf(stderr, "\n");
            }
        } else {
            fprintf(stderr, "  <ring not readable via process_vm_readv>\n");
        }
    }

    if (!qs->primed) {
        // Start from the current head so we only report new activity.
        qs->next_id = wptr;
        qs->last_rptr = rptr;
        qs->primed = true;
        return;
    }

    if (kDebug && (wptr != qs->next_id || rptr != qs->last_rptr)) {
        fprintf(stderr,
                "[sdma q%lu] wptr=%lu rptr=%lu next=%lu inflight=%zu "
                "ring_dw=%lu\n",
                q.uid, wptr, rptr, qs->next_id, qs->sdma_inflight.size(),
                ring_dw);
    }

    // Producer wrapped past what we have yet to read: skip ahead a whole ring.
    if (wptr > qs->next_id + ring_dw) {
        qs->dropped += (wptr - qs->next_id) - ring_dw;
        qs->next_id = wptr - ring_dw;
    }

    while (qs->next_id < wptr) {
        uint64_t avail = wptr - qs->next_id;
        uint32_t win =
            static_cast<uint32_t>(std::min<uint64_t>(avail, kSdmaWindow));
        uint32_t dws[kSdmaWindow];
        if (!ReadRingDwords(qs, qs->next_id, win, dws)) {
            if (kDebug)
                fprintf(stderr, "[sdma q%lu] read failed @dw%lu\n", q.uid,
                        qs->next_id);
            break;
        }

        uint32_t len = sdma::PacketLenDwords(dws, win);
        if (len == 0) {
            // avail < 4 -> the length-bearing dword may not be published yet;
            // wait for the next poll. Otherwise this is an unrecognised opcode
            // and we have lost sync -- drop to the current head and resync.
            if (avail < 4)
                break;
            if (kDebug)
                fprintf(stderr,
                        "[sdma q%lu] unknown op 0x%02x @dw%lu, resync to %lu\n",
                        q.uid, sdma::HeaderOp(dws[0]), qs->next_id, wptr);
            qs->dropped += avail;
            qs->next_id = wptr;
            break;
        }
        if (len > avail)
            break; // packet not fully published yet; retry next poll

        SdmaRecord rec;
        rec.queue_uid = q.uid;
        rec.seq = qs->sdma_seq++;
        rec.opcode = sdma::HeaderOp(dws[0]);
        rec.sub_opcode = sdma::HeaderSubOp(dws[0]);
        if (rec.opcode == sdma::OP_COPY) {
            rec.op_name =
                std::string("copy_") + sdma::CopySubOpName(rec.sub_opcode);
            if (rec.sub_opcode == sdma::SUBOP_COPY_LINEAR && win >= 7) {
                rec.bytes = sdma::LinearCopyBytes(dws[1]);
                rec.src_addr = dws[3] | (static_cast<uint64_t>(dws[4]) << 32);
                rec.dst_addr = dws[5] | (static_cast<uint64_t>(dws[6]) << 32);
                rec.dir = ClassifyDir(qs, rec.src_addr, rec.dst_addr);
            }
        } else {
            rec.op_name = sdma::OpName(rec.opcode);
        }
        rec.submit_ts = now;

        uint64_t end_dw = qs->next_id + len;
        qs->sdma_inflight.emplace(end_dw, std::move(rec));
        qs->next_id = end_dw;
    }

    // Mark packets queue-consumed once the read pointer passes their last
    // dword.
    while (!qs->sdma_inflight.empty()) {
        auto it = qs->sdma_inflight.begin();
        if (it->first > rptr)
            break;
        SdmaRecord& r = it->second;
        r.complete_ts = now;
        r.completed = true;
        if (sdma_sink_)
            sdma_sink_(r);
        qs->sdma_inflight.erase(it);
    }
    qs->last_rptr = rptr;
}

} // namespace hsasnoop
