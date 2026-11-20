#include "parser.h"

#include <sys/uio.h>
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

// Largest SDMA-packet dword window we read up front to decode header + length
// and (for linear copies) the src/dst addresses. Big WRITE payloads exceed this
// but their length is derived from the count dword within the window, so we can
// still stride past them correctly.
constexpr uint32_t kSdmaWindow = 16;

// Bound the direction-classification page cache so a long-lived --all daemon
// cannot grow it without limit.
constexpr size_t kPageCacheMax = 1u << 16;

// amd_signal_s is 64-byte aligned, so bits 0-5 of a valid handle are normally
// zero. Newer CP firmware uses bits 4-5 as per-dispatch timestamp hint flags
// (AMD_SIGNAL_HINT_TS_ON / TS_OFF / TS_INHERIT). Strip them before using the
// handle as a VA so reads land at the correct struct address regardless.
constexpr uint64_t kSignalVaMask = ~0x3FULL;

// Read amd_signal_s::value from a completion signal handle. The handle is a
// user VA pointing to amd_signal_s; value (volatile int64_t) lives at offset 8
// after the 8-byte kind field. The GPU decrements this to 0 at kernel
// completion, strictly at or before the CP advances rptr.
bool ReadSignalValue(int pid, uint64_t signal_handle, int64_t* out) {
    constexpr uint64_t kValueOffset = 8; // offsetof(amd_signal_s, value)
    uint64_t va = (signal_handle & kSignalVaMask) + kValueOffset;
    if (ReadProcMem(pid, va, out, sizeof(*out)))
        return true;
    return ReadProcMemViaMem(pid, va, out, sizeof(*out));
}

// Read amd_signal_s::start_ts and end_ts (GPU shader-clock ticks written by
// the CP when hsa_amd_profiling_set_profiler_enabled was active). start_ts is
// at offset 32, end_ts at offset 40. Returns false if either read fails or
// start_ts is 0 (profiling not enabled for this dispatch).
bool TryReadGpuTs(int pid, uint64_t signal_handle, uint64_t* start_ticks,
                  uint64_t* end_ticks) {
    constexpr uint64_t kStartOffset = 32; // offsetof(amd_signal_s, start_ts)
    constexpr uint64_t kEndOffset = 40;   // offsetof(amd_signal_s, end_ts)
    const uint64_t base = signal_handle & kSignalVaMask;
    auto read64 = [&](uint64_t off, uint64_t* out) -> bool {
        uint64_t va = base + off;
        if (ReadProcMem(pid, va, out, sizeof(*out)))
            return true;
        return ReadProcMemViaMem(pid, va, out, sizeof(*out));
    };
    uint64_t s = 0, e = 0;
    const bool read_ok = read64(kStartOffset, &s) && read64(kEndOffset, &e);
    if (kDebug)
        fprintf(stderr,
                "[gputs pid=%d] signal=0x%lx read=%s start_ts=%lu end_ts=%lu\n",
                pid, signal_handle, read_ok ? "ok" : "FAILED", s, e);
    // An end at or before the start is a signal caught between dispatches.
    if (!read_ok || s == 0 || e <= s)
        return false;
    *start_ticks = s;
    *end_ticks = e;
    return true;
}

void FillDispatch(const uint8_t* raw, PacketRecord* rec) {
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
}

void FillExtDispatch(const uint8_t* raw, PacketRecord* rec) {
    aql::ExtKernelDispatchPacket p;
    memcpy(&p, raw, sizeof(p));
    rec->kernel_object = p.kernel_object;
    rec->grid[0] = aql::ExtGridSize(p.cluster_count_x, p.cluster_size_x,
                                    p.workgroup_size_x);
    rec->grid[1] = aql::ExtGridSize(p.cluster_count_y, p.cluster_size_y,
                                    p.workgroup_size_y);
    rec->grid[2] = aql::ExtGridSize(p.cluster_count_z, p.cluster_size_z,
                                    p.workgroup_size_z);
    rec->wg[0] = p.workgroup_size_x;
    rec->wg[1] = p.workgroup_size_y;
    rec->wg[2] = p.workgroup_size_z;
    rec->private_seg = p.private_segment_size;
    rec->group_seg = p.group_segment_size;
    rec->completion_signal = p.completion_signal;
    rec->kernarg_address = p.kernarg_address;
}

// Idle backoff. A queue is polled at the full rate while it has activity and
// for kIdleHoldSec after; its interval then grows with the time it has been
// idle, up to the configured ceiling.
constexpr int kDefaultPollIdleUs = 1000;
constexpr double kIdleHoldSec = 0.010;
constexpr double kIdleGrowthDivisor = 8.0;
// A dead process is also caught by its pointer reads failing, so the /proc
// check need not run on every pass.
constexpr double kAliveCheckSec = 0.1;

} // namespace

RingParser::RingParser(Sink sink, SdmaSink sdma_sink, int poll_us,
                       MissSink miss_sink, int poll_idle_us)
    : sink_(std::move(sink)), sdma_sink_(std::move(sdma_sink)),
      miss_sink_(std::move(miss_sink)), poll_us_(poll_us),
      poll_idle_us_(poll_idle_us < 0 ? std::max(poll_us, kDefaultPollIdleUs)
                                     : std::max(poll_us, poll_idle_us)) {
    running_ = true;
    thread_ = std::thread(&RingParser::Run, this);
}

RingParser::~RingParser() { Stop(); }

void RingParser::Stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& q : queues_)
        PrintSummary(*q);
}

// One line per queue when it stops being polled: what was decoded and what
// went by undecoded. Without it a trace-mode run gives no way to tell a quiet
// queue from one whose packets were missed.
void RingParser::PrintSummary(const QueueState& qs) {
    const QueueInfo& q = qs.info;
    if (!qs.primed)
        return;
    if (q.is_sdma()) {
        fprintf(stderr,
                "[summary] sdma queue uid=%lu pid=%d decoded=%lu "
                "skipped_dwords=%lu before_first_poll_dwords=%lu "
                "undecoded_dwords=%lu\n",
                q.uid, q.pid, qs.decoded, qs.dropped, qs.skipped_at_start,
                qs.last_wptr > qs.next_id ? qs.last_wptr - qs.next_id : 0);
        return;
    }
    fprintf(
        stderr,
        "[summary] aql queue uid=%lu pid=%d slots=%u decoded=%lu missed: "
        "overwritten=%lu consumed=%lu unreadable=%lu before_first_poll=%lu\n",
        q.uid, q.pid, q.num_slots(), qs.decoded, qs.missed_overwritten,
        qs.missed_consumed, qs.missed_unreadable, qs.skipped_at_start);
}

void RingParser::AddQueue(const QueueInfo& q) {
    if (q.is_sdma() && !sdma::IsSupportedVersion(q.sdma_version)) {
        fprintf(stderr,
                "[parser] ignoring sdma queue uid=%lu gpu=%u: unsupported "
                "SDMA %s\n",
                q.uid, q.gpu_id, sdma::VersionName(q.sdma_version));
        return;
    }
    auto qs = std::make_unique<QueueState>();
    qs->info = q;
    // Kernel-name resolution only applies to AQL dispatch packets; SDMA copies
    // carry no kernel object, so skip the (process-scanning) resolver for them.
    if (q.is_aql())
        qs->ksym = std::make_unique<KernelSymbolResolver>(q.pid);
    std::lock_guard<std::mutex> lk(mu_);
    queues_.push_back(std::move(qs));
}

// Read ring memory. A ring normally lives in host (GTT) memory that
// process_vm_readv can reach. On ROCm 10.2 it can fail with EFAULT: rings are
// in device memory by default on gfx94x/gfx125x with Large BAR, and gfx1201
// rings are unreadable too for a reason not yet identified. /proc/<pid>/mem
// reads them in both cases, so fall back to it as ReadQueuePtr does.
bool RingParser::ReadRing(QueueState* qs, uint64_t va, void* out, size_t len) {
    const int pid = qs->info.pid;
    if (!qs->ring_via_mem) {
        if (ReadProcMem(pid, va, out, len))
            return true;
        if (!ReadProcMemViaMem(pid, va, out, len))
            return false;
        qs->ring_via_mem = true;
        fprintf(stderr,
                "[parser] queue uid=%lu pid=%d: ring is not readable with "
                "process_vm_readv; reading it through "
                "/proc/%d/mem\n",
                qs->info.uid, pid, pid);
        return true;
    }
    return ReadProcMemViaMem(pid, va, out, len);
}

// Read and decode the AQL packet at ring slot for `id`. Returns false if the
// packet is not yet ready (producer reserved the slot but has not published the
// header) or the read failed.
//
// rptr is passed so we can handle the WSL2/APU fast-reset case: on gfx1151
// (Strix Halo) the GPU resets the header to Invalid immediately on consumption,
// faster than our poll interval. When id < rptr (slot already consumed) and the
// header reads as Invalid but kernel_object is non-zero, the packet body is
// intact and we synthesize a KernelDispatch record rather than silently
// dropping it.
bool RingParser::DecodeSlot(QueueState* qs, uint64_t id, uint64_t rptr,
                            PacketRecord* rec, bool* read_failed) {
    const QueueInfo& q = qs->info;
    *read_failed = false;
    uint32_t slots = q.num_slots();
    if (!slots)
        return false;
    uint64_t addr = q.ring_base + (id % slots) * aql::kPacketSize;

    uint8_t raw[aql::kPacketSize];
    if (!ReadRing(qs, addr, raw, sizeof(raw))) {
        if (kDebug)
            fprintf(stderr, "[decode q%lu id%lu] read failed @0x%lx\n", q.uid,
                    id, addr);
        *read_failed = true;
        return false;
    }

    uint16_t header;
    memcpy(&header, raw, sizeof(header));
    aql::PacketType type = aql::HeaderType(header);

    // Producer publishes the header (with release) last. Treat
    // not-yet-published slots as pending so we don't decode a half-written
    // packet.
    //
    // Exception: on WSL2/APU (gfx1151, Strix Halo) the GPU resets the header
    // to Invalid immediately after consumption, before our poll loop can
    // observe it. If id < rptr the slot was committed and consumed; if
    // kernel_object at the expected offset is non-zero the dispatch body is
    // intact and we decode it as KernelDispatch.
    if (type == aql::PacketType::Invalid) {
        if (id < rptr) {
            // Slot consumed; check if kernel_object is set (dispatch body
            // intact). kernel_object sits at the same offset in both dispatch
            // layouts.
            aql::KernelDispatchPacket p;
            memcpy(&p, raw, sizeof(p));
            // workgroup_size_x (bytes 4-5 in both dispatch layouts) is at
            // least 1 in a real dispatch and reserved-zero in barrier packets.
            // Without it a consumed barrier_value, whose condition field sits
            // where kernel_object does, would be reported as a kernel launch.
            if (p.kernel_object != 0 && p.workgroup_size_x != 0) {
                // The header no longer says which layout the body has. A
                // classic dispatch has a 16-bit setup field whose high byte
                // (raw[3]) is always zero; an extended dispatch has its AMD
                // format in raw[2] and its own non-zero setup in raw[3].
                const bool ext =
                    aql::IsExtKernelDispatch(raw[2]) && raw[3] != 0;
                if (kDebug)
                    fprintf(stderr,
                            "[decode q%lu id%lu] fast-reset: synthesising "
                            "%sKernelDispatch kernel_obj=0x%lx\n",
                            q.uid, id, ext ? "Ext" : "", p.kernel_object);
                rec->queue_uid = q.uid;
                rec->dispatch_id = id;
                rec->type = aql::PacketType::KernelDispatch;
                rec->barrier = false;
                if (ext)
                    FillExtDispatch(raw, rec);
                else
                    FillDispatch(raw, rec);
                rec->kernel_name = qs->ksym->Resolve(rec->kernel_object);
                return true;
            }
        }
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
        FillDispatch(raw, rec);
        rec->kernel_name = qs->ksym->Resolve(rec->kernel_object);
    } else if (type == aql::PacketType::VendorSpecific &&
               aql::IsExtKernelDispatch(raw[2])) {
        // AMD extended kernel dispatch: a kernel launch in a vendor packet.
        // Report it as the KernelDispatch it is, so it gets a kernel name.
        rec->type = aql::PacketType::KernelDispatch;
        FillExtDispatch(raw, rec);
        rec->kernel_name = qs->ksym->Resolve(rec->kernel_object);
    } else if (type == aql::PacketType::VendorSpecific &&
               raw[2] == aql::kAmdFormatBarrierValue) {
        rec->kernel_name = "barrier_value";
    } else {
        rec->kernel_name = aql::PacketTypeName(type);
    }
    return true;
}

// Read a queue's write and read pointers. Normally one process_vm_readv call
// fetches both. Where that cannot reach them (DRM-backed pointer pages on
// RDNA4 and others) both are read through /proc/<pid>/mem instead, and that
// choice sticks so the failing call is not repeated on every poll.
bool RingParser::ReadPtrs(QueueState* qs, uint64_t* wptr, uint64_t* rptr) {
    const QueueInfo& q = qs->info;
    bool ok = false;
    if (!qs->ptr_via_mem) {
        struct iovec local[2] = {{wptr, sizeof(*wptr)}, {rptr, sizeof(*rptr)}};
        struct iovec remote[2] = {
            {reinterpret_cast<void*>(q.wptr_addr), sizeof(*wptr)},
            {reinterpret_cast<void*>(q.rptr_addr), sizeof(*rptr)}};
        // Anything short of both values counts as a failure: a partial read
        // returns the bytes it got and leaves errno alone.
        ok = process_vm_readv(q.pid, local, 2, remote, 2, 0) ==
             static_cast<ssize_t>(sizeof(*wptr) + sizeof(*rptr));
        if (!ok) {
            ok = ReadU64ViaMem(q.pid, q.wptr_addr, wptr) &&
                 ReadU64ViaMem(q.pid, q.rptr_addr, rptr);
            qs->ptr_via_mem = ok;
        }
    } else {
        ok = ReadU64ViaMem(q.pid, q.wptr_addr, wptr) &&
             ReadU64ViaMem(q.pid, q.rptr_addr, rptr);
        if (!ok)
            qs->ptr_via_mem = false; // try both paths again next time
    }
    if (!ok)
        return false;
    qs->moved =
        !qs->ptrs_seen || *wptr != qs->seen_wptr || *rptr != qs->seen_rptr;
    qs->ptrs_seen = true;
    qs->seen_wptr = *wptr;
    qs->seen_rptr = *rptr;
    return true;
}

void RingParser::PollQueue(QueueState* qs, double now) {
    const QueueInfo& q = qs->info;
    uint64_t wptr = 0, rptr = 0;
    if (!ReadPtrs(qs, &wptr, &rptr)) {
        // A process that has exited is the usual reason; drop its queue
        // quietly instead of reporting it as stale 50 polls later.
        if (!ProcAlive(q.pid)) {
            qs->evict = true;
            return;
        }
        // On WSL2 --all mode, PID reuse can leave queue records with VAs that
        // are invalid in the new process. Evict after sustained read failures.
        // Threshold 50 (not 5): gfx1250/RDNA4 reads transiently fail between
        // dispatch bursts; 5 was too aggressive and caused premature eviction.
        if (++qs->read_fail_count >= 50) {
            fprintf(stderr,
                    "[parser] evicting stale queue uid=%lu pid=%d "
                    "(wptr/rptr unreadable after %d attempts)\n",
                    q.uid, q.pid, qs->read_fail_count);
            qs->evict = true;
        }
        return;
    }
    qs->read_fail_count = 0; // clear on success
    qs->last_wptr = wptr;

    const uint64_t slots = q.num_slots();
    if (!qs->primed) {
        qs->last_rptr = rptr;
        qs->primed = true;
        // A queue seen being created has nothing in its ring from before the
        // trace, so everything up to wptr is new activity: the runtime can
        // enqueue on it before the first poll gets here. Start from the
        // beginning and decode on this pass; if the ring has already wrapped,
        // the guard below counts what was overwritten. A queue found already
        // running starts from the current head so only new activity is
        // reported.
        if (!q.created_during_trace) {
            qs->next_id = wptr;
            qs->skipped_at_start = wptr;
            return;
        }
        qs->next_id = 0;
    }

    if (kDebug && (wptr != qs->next_id || rptr != qs->last_rptr)) {
        fprintf(stderr,
                "[poll q%lu] wptr=%lu rptr=%lu next_id=%lu inflight=%zu\n",
                q.uid, wptr, rptr, qs->next_id, qs->inflight.size());
    }

    // Guard against decoding slots the producer has already wrapped over.
    uint64_t overwritten = 0, consumed_undecodable = 0, unreadable = 0;
    if (wptr > qs->next_id + slots) {
        overwritten = (wptr - qs->next_id) - slots;
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
        bool read_failed = false;
        if (DecodeSlot(qs, qs->next_id, rptr, &rec, &read_failed)) {
            rec.submit_ts = now;
            // Already consumed by the CP when first seen (id < rptr). Record
            // when, and let the retire loop below decide whether it has
            // finished or is still waiting on its completion signal.
            if (qs->next_id < rptr)
                rec.consumed_ts = now;
            qs->inflight.emplace(qs->next_id, std::move(rec));
            qs->next_id++;
            qs->decoded++;
        } else if (qs->next_id < rptr) {
            ++(read_failed ? unreadable : consumed_undecodable);
            qs->next_id++;
        } else {
            break; // genuinely pending publication
        }
    }
    qs->dropped += overwritten + consumed_undecodable + unreadable;
    qs->missed_overwritten += overwritten;
    qs->missed_consumed += consumed_undecodable;
    qs->missed_unreadable += unreadable;
    if (miss_sink_) {
        if (overwritten)
            miss_sink_(q.uid, overwritten, "overwritten");
        if (consumed_undecodable)
            miss_sink_(q.uid, consumed_undecodable, "consumed");
        if (unreadable)
            miss_sink_(q.uid, unreadable, "unreadable");
    }

    // Retire inflight packets in dispatch-id order.
    //
    // rptr advancing past a packet means the CP has taken it off the queue,
    // not that the kernel has finished. A packet with a completion signal is
    // finished when the signal value drops to <= 0, which is also when the CP
    // has written the dispatch's start/end ticks into the signal. So a packet
    // with a readable signal retires on the signal, and one without retires
    // on rptr.
    //
    // A signal that never drops (recycled by the runtime before this poller
    // saw it at <= 0) would hold the queue's records back indefinitely, so a
    // consumed packet stops waiting after kSignalWaitSec and retires on rptr.
    constexpr double kSignalWaitSec = 5.0;
    while (!qs->inflight.empty()) {
        auto it = qs->inflight.begin();
        PacketRecord& r = it->second;
        const bool consumed = it->first < rptr;
        if (consumed && r.consumed_ts == 0)
            r.consumed_ts = now;

        bool signalled = false, awaiting_signal = false;
        if (r.completion_signal != 0) {
            int64_t sig = 0;
            if (ReadSignalValue(q.pid, r.completion_signal, &sig)) {
                signalled = sig <= 0;
                awaiting_signal = !signalled;
            }
        }
        if (!consumed && !signalled)
            break;
        const bool timed_out =
            awaiting_signal && now - r.consumed_ts >= kSignalWaitSec;
        if (awaiting_signal && !timed_out)
            break;
        if (kDebug && r.completion_signal != 0)
            fprintf(stderr, "[poll q%lu id%lu] completed via %s (rptr %lu)\n",
                    q.uid, it->first,
                    signalled ? "signal" : "rptr after signal timeout", rptr);
        r.complete_ts = timed_out ? r.consumed_ts : now;
        r.completed = true;
        // Only a signal seen fired holds this dispatch's timestamps. One that
        // timed out was reused by the runtime, and its ticks belong to a
        // later dispatch.
        if (signalled)
            TryReadGpuTs(q.pid, r.completion_signal, &r.gpu_start_ticks,
                         &r.gpu_end_ticks);
        if (sink_)
            sink_(r);
        qs->inflight.erase(it);
    }
    qs->last_rptr = rptr;
}

void RingParser::Run() {
    while (running_) {
        std::vector<QueueState*> snapshot;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& q : queues_)
                snapshot.push_back(q.get());
        }
        // When the next queue is due. Stays at "now" while any queue is
        // active, which keeps the loop at the full poll rate.
        double next_due = -1;
        bool any_active = false;
        for (QueueState* qs : snapshot) {
            if (qs->evict)
                continue;
            // Timestamp per queue, not per pass: a slow pass over one queue
            // must not be charged to the next queue's durations.
            const double now = MonoNow();
            if (now < qs->next_poll_ts) {
                if (next_due < 0 || qs->next_poll_ts < next_due)
                    next_due = qs->next_poll_ts;
                continue;
            }
            if (now >= qs->next_alive_ts) {
                if (!ProcAlive(qs->info.pid)) {
                    qs->evict = true;
                    continue;
                }
                qs->next_alive_ts = now + kAliveCheckSec;
            }
            qs->moved = false;
            if (qs->info.is_sdma())
                PollSdmaQueue(qs, now);
            else
                PollQueue(qs, now);
            queue_polls_.fetch_add(1, std::memory_order_relaxed);

            // Never back off with work outstanding: packets in flight, a
            // backlog not yet decoded, or reads that are failing.
            const bool busy = qs->moved || qs->read_fail_count > 0 ||
                              !qs->inflight.empty() ||
                              !qs->sdma_inflight.empty() ||
                              (qs->primed && qs->next_id < qs->last_wptr);
            if (busy || qs->last_active_ts == 0)
                qs->last_active_ts = now;
            const double idle_for = now - qs->last_active_ts;
            if (poll_idle_us_ <= poll_us_ || idle_for < kIdleHoldSec) {
                qs->next_poll_ts = 0;
                any_active = true;
                continue;
            }
            const double interval = std::min(
                poll_idle_us_ * 1e-6,
                std::max(poll_us_ * 1e-6, idle_for / kIdleGrowthDivisor));
            qs->next_poll_ts = now + interval;
            if (next_due < 0 || qs->next_poll_ts < next_due)
                next_due = qs->next_poll_ts;
        }
        // Remove evicted queues under the lock.
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto& q : queues_)
                if (q->evict)
                    PrintSummary(*q);
            queues_.erase(
                std::remove_if(queues_.begin(), queues_.end(),
                               [](const std::unique_ptr<QueueState>& q) {
                                   return q->evict;
                               }),
                queues_.end());
        }
        // Sleep one poll interval while anything is active. Otherwise sleep
        // until the first idle queue is due, or for the idle ceiling when
        // there is nothing to poll at all, so a queue added meanwhile waits
        // no longer than that.
        double sleep_us = poll_us_;
        if (!any_active) {
            sleep_us = poll_idle_us_;
            if (next_due >= 0)
                sleep_us = std::min<double>(
                    sleep_us,
                    std::max<double>(poll_us_, (next_due - MonoNow()) * 1e6));
        }
        usleep(static_cast<useconds_t>(sleep_us));
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
    if (!ReadRing(qs, q.ring_base + off * 4, out, first * 4))
        return false;
    if (first < n)
        return ReadRing(qs, q.ring_base, out + first, (n - first) * 4);
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
    if (!ReadPtrs(qs, &wptr_raw, &rptr_raw)) {
        if (!ProcAlive(q.pid)) {
            qs->evict = true;
            return;
        }
        if (++qs->read_fail_count >= 50) {
            fprintf(stderr,
                    "[parser] evicting stale sdma queue uid=%lu pid=%d "
                    "(wptr/rptr unreadable after %d attempts)\n",
                    q.uid, q.pid, qs->read_fail_count);
            qs->evict = true;
        }
        return;
    }

    qs->read_fail_count = 0; // clear on success
    uint64_t ring_dw = q.num_dwords();
    if (!ring_dw)
        return;

    // KFD/ROCR publish SDMA queue pointers as byte offsets. Normalise them to
    // the dword indices used by the packet decoder and ring-size arithmetic.
    uint64_t wptr = sdma::RingBytesToDwords(wptr_raw);
    uint64_t rptr = sdma::RingBytesToDwords(rptr_raw);
    qs->last_wptr = wptr;

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
        qs->last_rptr = rptr;
        qs->primed = true;
        // Same rule as for AQL queues: one seen being created is decoded from
        // the start of its ring, one found already running from the current
        // head.
        if (!q.created_during_trace) {
            qs->next_id = wptr;
            qs->skipped_at_start = wptr;
            return;
        }
        qs->next_id = 0;
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

        uint32_t len = sdma::PacketLenDwords(q.sdma_version, dws, win);
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
        rec.sdma_version = q.sdma_version;
        if (rec.opcode == sdma::OP_COPY) {
            rec.op_name = std::string("copy_") +
                          sdma::CopySubOpName(q.sdma_version, rec.sub_opcode);
            if (sdma::HeaderBroadcast(dws[0]))
                rec.op_name += "_broadcast";
            // The copy block (count, parameters, source, destination)
            // follows the header directly, or a wait block on gfx12.5+.
            const uint32_t body = sdma::CopyBodyOffset(q.sdma_version, dws[0]);
            if (sdma::IsLinearCopy(q.sdma_version, rec.sub_opcode) &&
                win >= body + 6) {
                rec.bytes = sdma::LinearCopyBytes(q.sdma_version,
                                                  rec.sub_opcode, dws[body]);
                rec.src_addr = dws[body + 2] |
                               (static_cast<uint64_t>(dws[body + 3]) << 32);
                rec.dst_addr = dws[body + 4] |
                               (static_cast<uint64_t>(dws[body + 5]) << 32);
                // An indirect copy names a cell that holds the address.
                // Follow it; if the cell cannot be read, the direction is
                // left unknown rather than taken from the cell's own address.
                bool resolved = true;
                if (q.sdma_version == sdma::Version::V7Gfx125) {
                    const bool isrc = sdma::HeaderIndirectSrc(dws[0]);
                    const bool idst = sdma::HeaderIndirectDst(dws[0]);
                    if (isrc || idst)
                        rec.op_name += "_indirect";
                    if (isrc)
                        resolved &= ReadU64(q.pid, rec.src_addr, &rec.src_addr);
                    if (idst)
                        resolved &= ReadU64(q.pid, rec.dst_addr, &rec.dst_addr);
                }
                if (resolved)
                    rec.dir = ClassifyDir(qs, rec.src_addr, rec.dst_addr);
            }
        } else {
            rec.op_name = sdma::OpName(q.sdma_version, rec.opcode);
        }
        rec.submit_ts = now;

        uint64_t end_dw = qs->next_id + len;
        qs->sdma_inflight.emplace(end_dw, std::move(rec));
        qs->next_id = end_dw;
        qs->decoded++;
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
