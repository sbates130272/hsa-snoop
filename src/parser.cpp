#include "parser.h"

#include <sys/timerfd.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

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
// How late a poll may be before it counts as a stall: this, or the queue's
// own interval if that is longer. Above what a pass over many busy queues or
// a scan for kernel names costs; far below what it takes to lose data.
constexpr double kStallSlackSec = 0.050;
// How long before a queue went away its last successful poll may have been.
// A runtime shutting down routinely leaves 10 to 40 ms here with nothing
// lost; flagging those made the counter fire on one queue in ten.
constexpr double kTailSlackSec = 0.050;
constexpr double kStallLogSec = 5.0;
// Consecutive failed pointer reads before a queue is given up on. 50 (not
// 5): gfx1250/RDNA4 reads fail transiently between dispatch bursts, and 5
// evicted queues that were still in use.
constexpr int kMaxReadFails = 50;
// Least time between two looks at whether a queue's pointer page is still
// mapped while its reads are failing.
constexpr double kMapCheckSec = 0.1;
// Sleeps this long or longer are taken on the thread's condition variable,
// so that a new queue or Stop() ends them.
constexpr double kWakeableSleepUs = 1000;

std::atomic<void (*)(int)> watch_read_hook{nullptr};

} // namespace

void RingParser::SetWatchReadHookForTest(void (*hook)(int pid)) {
    watch_read_hook.store(hook);
}

int RingParser::DefaultPollThreads() {
    const unsigned hw = std::thread::hardware_concurrency();
    return static_cast<int>(std::min(4u, std::max(1u, hw)));
}

RingParser::RingParser(Sink sink, SdmaSink sdma_sink, int poll_us,
                       MissSink miss_sink, int poll_idle_us,
                       RemoveSink remove_sink, int poll_threads,
                       StallSink stall_sink)
    : sink_(std::move(sink)), sdma_sink_(std::move(sdma_sink)),
      miss_sink_(std::move(miss_sink)), remove_sink_(std::move(remove_sink)),
      stall_sink_(std::move(stall_sink)), poll_us_(poll_us),
      poll_idle_us_(poll_idle_us < 0 ? std::max(poll_us, kDefaultPollIdleUs)
                                     : std::max(poll_us, poll_idle_us)) {
    if (poll_threads <= 0)
        poll_threads = DefaultPollThreads();
    poll_threads = std::min(poll_threads, kMaxPollThreads);
    running_ = true;
    for (int i = 0; i < poll_threads; ++i) {
        workers_.push_back(std::make_unique<Worker>());
        workers_.back()->index = i;
    }
    watch_ = workers_.size() > 1 && poll_idle_us_ > poll_us_;
    for (auto& w : workers_)
        w->thread = std::thread(&RingParser::Run, this, w.get());
    if (watch_) {
        // Without the timer the watch thread is merely unsupervised.
        watchdog_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
        if (watchdog_fd_ >= 0)
            watchdog_thread_ = std::thread(&RingParser::Watchdog, this);
        watch_thread_ = std::thread(&RingParser::Watch, this);
    }
}

RingParser::~RingParser() { Stop(); }

double RingParser::StalledSeconds() const {
    double total = 0;
    for (const auto& w : workers_)
        total += w->stalled_sec.load();
    return total;
}

void RingParser::Stop() {
    if (!running_.exchange(false))
        return;
    for (auto& w : workers_) {
        // Taken so the wake-up cannot fall between a worker's check of
        // running_ and its wait.
        { std::lock_guard<std::mutex> lk(w->mu); }
        w->cv.notify_all();
    }
    { std::lock_guard<std::mutex> lk(watch_mu_); }
    watch_cv_.notify_all();
    if (watch_thread_.joinable())
        watch_thread_.join();
    if (watchdog_thread_.joinable()) {
        ArmWatchdog(1); // fires at once; the thread sees running_ is false
        watchdog_thread_.join();
    }
    if (watchdog_fd_ >= 0) {
        close(watchdog_fd_);
        watchdog_fd_ = -1;
    }
    for (auto& w : workers_)
        if (w->thread.joinable())
            w->thread.join();
    std::string per_thread;
    for (auto& w : workers_) {
        std::lock_guard<std::mutex> lk(w->mu);
        for (const auto& q : w->queues)
            PrintSummary(*q);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%.3f", per_thread.empty() ? "" : ",",
                 w->stalled_sec.load());
        per_thread += buf;
    }
    fprintf(stderr,
            "[summary] poller threads=%zu stalled_seconds=%.3f "
            "per_thread=%s queues_unobserved_tail=%lu\n",
            workers_.size(), StalledSeconds(), per_thread.c_str(),
            unobserved_tails_.load());
}

// One line per queue when it stops being polled: what was decoded and what
// went by undecoded. Without it a trace-mode run gives no way to tell a quiet
// queue from one whose packets were missed.
void RingParser::PrintSummary(const QueueState& qs) {
    const QueueInfo& q = qs.info;
    if (!qs.primed) {
        // A queue never read has nothing to report, unless it was there long
        // enough that it should have been. Not a "[summary] <kind> queue"
        // line: those carry decode counts, and this has none.
        if (qs.tail_unobserved)
            fprintf(stderr,
                    "[summary] unread %s queue uid=%lu pid=%d "
                    "tail_unobserved=1 unobserved_ms=%.1f\n",
                    q.is_sdma() ? "sdma" : "aql", q.uid, q.pid,
                    qs.unobserved_sec * 1e3);
        return;
    }
    char health[96];
    snprintf(health, sizeof(health),
             "polls=%lu max_poll_gap_ms=%.1f tail_unobserved=%d", qs.ok_polls,
             qs.max_gap * 1e3, qs.tail_unobserved ? 1 : 0);
    if (q.is_sdma()) {
        fprintf(stderr,
                "[summary] sdma queue uid=%lu pid=%d decoded=%lu "
                "skipped_dwords=%lu before_first_poll_dwords=%lu "
                "undecoded_dwords=%lu %s\n",
                q.uid, q.pid, qs.decoded, qs.dropped, qs.skipped_at_start,
                qs.last_wptr > qs.next_id ? qs.last_wptr - qs.next_id : 0,
                health);
        return;
    }
    fprintf(stderr,
            "[summary] aql queue uid=%lu pid=%d slots=%u decoded=%lu missed: "
            "overwritten=%lu consumed=%lu unreadable=%lu before_first_poll=%lu "
            "%s\n",
            q.uid, q.pid, q.num_slots(), qs.decoded, qs.missed_overwritten,
            qs.missed_consumed, qs.missed_unreadable, qs.skipped_at_start,
            health);
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
    qs->last_poll_ts = qs->last_ok_ts = MonoNow();
    // By pid, so that everything of one process is polled, and its records
    // delivered, by one thread and in order.
    Worker* w = workers_[static_cast<unsigned>(q.pid) % workers_.size()].get();
    {
        std::lock_guard<std::mutex> lk(w->mu);
        w->queues.push_back(std::move(qs));
        w->kick = true;
    }
    w->cv.notify_all();
}

// Add the stretch [from, to] to this thread's stalled time, less whatever
// part of it has been counted already: queues overdue through the same
// stretch must count it once. True when a log line is due.
bool RingParser::CountStall(Worker* w, double from, double to) {
    const double add = to - std::max(from, w->stall_counted_until);
    w->stall_counted_until = std::max(to, w->stall_counted_until);
    if (add <= 0)
        return false;
    w->stalled_sec.store(w->stalled_sec.load() + add);
    if (stall_sink_)
        stall_sink_(w->index, add);
    w->stall_unlogged += add;
    return to - w->stall_log_ts >= kStallLogSec;
}

void RingParser::LogStall(Worker* w, const QueueState& qs, double now,
                          bool late_start, double sec) {
    char detail[96];
    snprintf(detail, sizeof(detail),
             late_start ? "queue uid=%lu pid=%d was polled %.3f s late"
                        : "the last poll of queue uid=%lu pid=%d took %.3f s",
             qs.info.uid, qs.info.pid, sec);
    fprintf(stderr,
            "[parser] poll thread %u stalled for %.3f s (%.3f s in total); "
            "%s\n",
            w->index, w->stall_unlogged, w->stalled_sec.load(), detail);
    w->stall_unlogged = 0;
    w->stall_log_ts = now;
}

// Called as a queue is about to be polled. A poll that starts later than
// intended by more than the slack is a stall: this thread was held up, most
// often in a read of some queue's memory that blocked in the kernel, and
// none of its queues was looked at meanwhile.
void RingParser::NoteStall(Worker* w, QueueState* qs, double now) {
    const double gap = now - qs->last_poll_ts;
    qs->max_gap = std::max(qs->max_gap, gap);
    const double late = gap - qs->poll_interval;
    if (late <= std::max(kStallSlackSec, qs->poll_interval))
        return;
    if (CountStall(w, now - late, now))
        LogStall(w, *qs, now, true, late);
}

// Called when the poll that began at `start` has ended with the queue marked
// for removal. A stall is otherwise noticed when a later poll starts late;
// if this was the thread's last queue there is no later poll, and time the
// thread was held in this one (a read that blocked and then failed because
// the process had exited meanwhile) would go uncounted.
void RingParser::NoteHeld(Worker* w, QueueState* qs, double start) {
    const double now = MonoNow();
    const double held = now - start;
    qs->max_gap = std::max(qs->max_gap, held);
    if (held <= kStallSlackSec)
        return;
    if (CountStall(w, start, now))
        LogStall(w, *qs, now, false, held);
}

// Called once when a queue is marked for removal. Packets decoded but still
// waiting to be seen consumed or signalled are reported as they stand: with
// the queue gone there will be no later look at them. One the read pointer
// had passed completed no later than the last successful poll.
void RingParser::NoteDropped(QueueState* qs) {
    for (auto& [id, r] : qs->inflight) {
        if (id < qs->last_rptr) {
            r.complete_ts = r.consumed_ts ? r.consumed_ts : qs->last_ok_ts;
            r.completed = true;
        }
        if (sink_)
            sink_(r);
    }
    qs->inflight.clear();
    for (auto& [end_dw, r] : qs->sdma_inflight) {
        if (end_dw <= qs->last_rptr) {
            r.complete_ts = qs->last_ok_ts;
            r.completed = true;
        }
        if (sdma_sink_)
            sdma_sink_(r);
    }
    qs->sdma_inflight.clear();

    // Up to when the queue was found gone, not up to now: working out why
    // its pointers could not be read, and the sinks above, are this
    // program's own time and nothing of the queue's went unseen in it.
    const double since_ok = qs->gone_ts - qs->last_ok_ts;
    if (since_ok <= qs->ok_interval + std::max(kTailSlackSec, qs->ok_interval))
        return;
    qs->tail_unobserved = true;
    qs->unobserved_sec = since_ok;
    unobserved_tails_.fetch_add(1, std::memory_order_relaxed);
    fprintf(stderr,
            "[parser] queue uid=%lu pid=%d went away %.3f s after %s; what it "
            "did in that time was not seen and is not counted\n",
            qs->info.uid, qs->info.pid, since_ok,
            qs->ok_polls ? "its last successful poll" : "it was added, unread");
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

// Called when a pointer read has failed: true if that is because the queue
// has gone the normal way and should be dropped without comment. Either the
// process has exited (or is exiting and has given up its address space), or
// it is shutting down its runtime, which destroys each queue and unmaps its
// pointer page while the process is still very much alive. Pointers that
// were never readable, or that are still mapped, are not this case; nor is
// a process or mapping that could not be looked at.
//
// The mapping is looked up on the first failure of a run and when the queue
// is about to be given up on, and otherwise at most every kMapCheckSec:
// reads that fail for a while with the page still mapped must not cost a
// lookup each.
bool RingParser::QueueGone(QueueState* qs, double now, bool last_chance) {
    const QueueInfo& q = qs->info;
    if (!ProcAlive(q.pid))
        return true;
    if (!qs->ptrs_seen)
        return false;
    if (!last_chance && now < qs->next_map_check_ts)
        return false;
    qs->next_map_check_ts = now + kMapCheckSec;
    return AddressMapped(q.pid, q.wptr_addr) == Mapped::No;
}

// A pointer read has failed. Marks the queue for removal if it has gone, or
// if its pointers have been unreadable for too long.
void RingParser::PtrReadFailed(QueueState* qs) {
    const QueueInfo& q = qs->info;
    const double now = MonoNow();
    if (qs->read_fail_count == 0)
        qs->fail_since_ts = now;
    if (QueueGone(qs, now, qs->read_fail_count + 1 >= kMaxReadFails)) {
        // It was last readable before the first of these failures.
        qs->gone_ts = qs->fail_since_ts;
        qs->evict = true;
        return;
    }
    // On WSL2 --all mode, PID reuse can leave queue records with VAs that
    // are invalid in the new process. Evict after sustained read failures.
    if (++qs->read_fail_count >= kMaxReadFails) {
        fprintf(stderr,
                "[parser] evicting stale %squeue uid=%lu pid=%d "
                "(wptr/rptr unreadable after %d attempts)\n",
                q.is_sdma() ? "sdma " : "", q.uid, q.pid, qs->read_fail_count);
        qs->gone_ts = now;
        qs->evict = true;
    }
}

void RingParser::PollQueue(QueueState* qs, double now) {
    const QueueInfo& q = qs->info;
    uint64_t wptr = 0, rptr = 0;
    if (!ReadPtrs(qs, &wptr, &rptr)) {
        PtrReadFailed(qs);
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

void RingParser::Run(Worker* w) {
    while (running_) {
        std::vector<QueueState*> snapshot;
        {
            std::unique_lock<std::mutex> lk(w->mu);
            // A thread with no queues sleeps until it is given one.
            w->cv.wait(lk, [&] { return !running_ || !w->queues.empty(); });
            if (!running_)
                break;
            for (auto& q : w->queues)
                snapshot.push_back(q.get());
        }
        // When the next queue is due. Stays at "now" while any queue is
        // active, which keeps the loop at the full poll rate.
        double next_due = -1;
        bool any_active = false;
        bool any_evicted = false;
        // Every queue is at the idle ceiling: the watch thread can stand in.
        const double ceiling = poll_idle_us_ * 1e-6;
        bool parkable = watch_ && !watch_held_up_.load();
        for (QueueState* qs : snapshot) {
            if (qs->evict)
                continue;
            // Timestamp per queue, not per pass: a slow pass over one queue
            // must not be charged to the next queue's durations.
            const double now = MonoNow();
            if (now < qs->next_poll_ts) {
                if (next_due < 0 || qs->next_poll_ts < next_due)
                    next_due = qs->next_poll_ts;
                parkable = parkable && qs->poll_interval >= ceiling;
                continue;
            }
            NoteStall(w, qs, now);
            qs->last_poll_ts = now;
            qs->poll_interval = poll_us_ * 1e-6;
            if (now >= qs->next_alive_ts) {
                if (!ProcAlive(qs->info.pid)) {
                    qs->evict = true;
                    qs->gone_ts = now;
                    any_evicted = true;
                    NoteDropped(qs);
                    NoteHeld(w, qs, now);
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
            if (qs->evict) {
                any_evicted = true;
                NoteDropped(qs);
                NoteHeld(w, qs, now);
                continue;
            }
            const bool read_ok = qs->read_fail_count == 0;

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
            } else {
                qs->poll_interval = std::min(
                    poll_idle_us_ * 1e-6,
                    std::max(poll_us_ * 1e-6, idle_for / kIdleGrowthDivisor));
                // On a grid common to all queues with this interval, so that
                // a thread wakes once for its idle queues, not once for each.
                qs->next_poll_ts = (std::floor(now / qs->poll_interval) + 1) *
                                   qs->poll_interval;
                if (next_due < 0 || qs->next_poll_ts < next_due)
                    next_due = qs->next_poll_ts;
            }
            parkable = parkable && qs->poll_interval >= ceiling;
            if (read_ok) {
                qs->last_ok_ts = now;
                qs->ok_interval = qs->poll_interval;
                ++qs->ok_polls;
            }
        }
        // Remove evicted queues under the lock; tell the owner without it,
        // so the callback is free to call back into the parser.
        std::vector<std::unique_ptr<QueueState>> removed;
        if (any_evicted) {
            std::lock_guard<std::mutex> lk(w->mu);
            auto first_evicted = std::stable_partition(
                w->queues.begin(), w->queues.end(),
                [](const std::unique_ptr<QueueState>& q) { return !q->evict; });
            for (auto it = first_evicted; it != w->queues.end(); ++it) {
                PrintSummary(**it);
                removed.push_back(std::move(*it));
            }
            w->queues.erase(first_evicted, w->queues.end());
        }
        if (remove_sink_)
            for (const auto& q : removed)
                remove_sink_(q->info, q->tail_unobserved);
        removed.clear();
        if (parkable && !any_active && !any_evicted && next_due >= 0) {
            Park(w);
            continue;
        }
        // Sleep one poll interval while anything is active. Otherwise sleep
        // until the first idle queue is due, or for the idle ceiling when
        // there is nothing due at all.
        double sleep_us = poll_us_;
        if (!any_active) {
            sleep_us = poll_idle_us_;
            if (next_due >= 0)
                sleep_us = std::min<double>(
                    sleep_us,
                    std::max<double>(poll_us_, (next_due - MonoNow()) * 1e6));
        }
        if (sleep_us < kWakeableSleepUs) {
            usleep(static_cast<useconds_t>(sleep_us));
            continue;
        }
        // A queue added during a long sleep must not wait it out: it may be
        // in use already, and its late first poll would count as a stall.
        std::unique_lock<std::mutex> lk(w->mu);
        w->cv.wait_for(
            lk, std::chrono::microseconds(static_cast<int64_t>(sleep_us)),
            [&] { return !running_ || w->kick; });
        w->kick = false;
    }
    // A stall that lasted until Stop() has not been seen by any poll.
    std::lock_guard<std::mutex> lk(w->mu);
    const double now = MonoNow();
    for (auto& q : w->queues)
        if (now >= q->next_poll_ts)
            NoteStall(w, q.get(), now);
}

// Sleep while the watch thread looks at this thread's queues. Ends when the
// watch thread sees one of them move or fail to read, when a queue is added,
// when the watchdog finds the watch thread held up, or after a while
// regardless.
void RingParser::Park(Worker* w) {
    {
        std::lock_guard<std::mutex> lk(watch_mu_);
        ++parked_workers_;
    }
    watch_cv_.notify_all();
    const auto backstop = std::chrono::microseconds(
        std::max<int64_t>(20000, 4LL * poll_idle_us_));
    {
        std::unique_lock<std::mutex> lk(w->mu);
        if (!w->kick) {
            w->parked = true;
            ++w->park_gen;
            w->cv.wait_for(lk, backstop, [&] { return !running_ || w->kick; });
            w->parked = false;
        }
        w->kick = false;
        // Whatever ended the sleep, look at everything now.
        for (auto& q : w->queues)
            q->next_poll_ts = 0;
    }
    std::lock_guard<std::mutex> lk(watch_mu_);
    --parked_workers_;
}

// Set the watchdog timer to go off in `ns` nanoseconds; 0 turns it off.
void RingParser::ArmWatchdog(int64_t ns) {
    if (watchdog_fd_ < 0)
        return;
    struct itimerspec its {};
    its.it_value.tv_sec = ns / 1000000000;
    its.it_value.tv_nsec = ns % 1000000000;
    timerfd_settime(watchdog_fd_, 0, &its, nullptr);
}

// The watch thread is shared by all poll threads, and its reads can block in
// the kernel like any other. It pushes a timer back at the start of every
// pass; this thread sleeps until the timer goes off, which means a pass has
// not come round in several idle intervals. The parked poll threads are then
// woken and poll their own idle queues until the watch thread is back. It
// costs no wake-ups while all is well.
void RingParser::Watchdog() {
    for (;;) {
        uint64_t expirations = 0;
        const ssize_t n = read(watchdog_fd_, &expirations, sizeof(expirations));
        if (!running_)
            return;
        if (n != static_cast<ssize_t>(sizeof(expirations))) {
            if (errno == EINTR)
                continue;
            return;
        }
        watch_held_up_.store(true);
        for (auto& w : workers_) {
            std::lock_guard<std::mutex> lk(w->mu);
            if (w->parked) {
                w->kick = true;
                w->cv.notify_all();
            }
        }
    }
}

// The watch thread. Once per idle interval it reads the pointers of the
// queues of every parked poll thread. A queue that has moved, or cannot be
// read, gets its thread woken to deal with it; one that has not counts as
// polled. It keeps no state of its own and sinks nothing.
void RingParser::Watch() {
    struct Peek {
        int pid;
        uint64_t wptr_addr, rptr_addr, wptr, rptr;
        bool via_mem;
    };
    std::vector<Peek> peeks;
    while (running_) {
        // Back at the top, so not held up. Cleared before the wait: no poll
        // thread parks while it is set, and the wait is for one to park.
        watch_held_up_.store(false);
        {
            std::unique_lock<std::mutex> lk(watch_mu_);
            if (running_ && parked_workers_ == 0) {
                ArmWatchdog(0);
                watch_cv_.wait(
                    lk, [&] { return !running_ || parked_workers_ > 0; });
            }
        }
        // A pass, and the sleep after it, take little more than one idle
        // interval; several without a new pass means this thread is held up.
        ArmWatchdog(std::max<int64_t>(4000LL * poll_idle_us_, 2000000));
        for (auto& w : workers_) {
            uint64_t gen = 0;
            peeks.clear();
            {
                std::lock_guard<std::mutex> lk(w->mu);
                if (!w->parked || w->kick)
                    continue;
                gen = w->park_gen;
                // A parked thread is not touching its queues.
                for (const auto& q : w->queues)
                    peeks.push_back({q->info.pid, q->info.wptr_addr,
                                     q->info.rptr_addr, q->seen_wptr,
                                     q->seen_rptr, q->ptr_via_mem});
            }
            const double now = MonoNow();
            bool moved = false;
            void (*const hook)(int) = watch_read_hook.load();
            for (const Peek& p : peeks) {
                if (hook)
                    hook(p.pid);
                uint64_t wptr = 0, rptr = 0;
                const bool ok =
                    p.via_mem ? ReadU64ViaMem(p.pid, p.wptr_addr, &wptr) &&
                                    ReadU64ViaMem(p.pid, p.rptr_addr, &rptr)
                              : ReadU64(p.pid, p.wptr_addr, &wptr) &&
                                    ReadU64(p.pid, p.rptr_addr, &rptr);
                queue_polls_.fetch_add(1, std::memory_order_relaxed);
                if (!ok || wptr != p.wptr || rptr != p.rptr) {
                    moved = true;
                    break;
                }
            }
            std::lock_guard<std::mutex> lk(w->mu);
            if (!w->parked || w->park_gen != gen)
                continue;
            if (moved) {
                w->kick = true;
                w->cv.notify_all();
                continue;
            }
            for (const auto& q : w->queues) {
                q->last_poll_ts = q->last_ok_ts = now;
                ++q->ok_polls;
            }
        }
        usleep(static_cast<useconds_t>(poll_idle_us_));
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
        PtrReadFailed(qs);
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
    static std::atomic<int> dump_budget{16};
    if (kSdmaDump && wptr_raw != rptr_raw && dump_budget.fetch_sub(1) > 0) {
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
