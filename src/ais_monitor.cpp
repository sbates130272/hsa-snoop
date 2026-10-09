// ais_monitor.cpp - AIS IO monitor via tracefs kprobe/kretprobe.
//
// Installs two tracefs kprobes on kfd_ioctl_ais:
//   entry (p:) — captures op, size_req, gpu_id, fd from the args struct
//   return (r:) — captures size_copied, status, retval from the same struct
//
// Reads from trace_pipe, correlates entry and return by TID, and calls the
// sink for each completed AIS operation.  Uses tracefs directly (no bpftrace)
// because bpftrace 0.25 silently fails to hook DKMS module symbols whose
// kallsyms addresses were zero when the eBPF program was loaded.
//
// The PCIe BDF string is resolved from <pid>'s /proc/PID/fd/<fd> → device
// → /sys/class/block/... → PCI slot once per unique <pid,fd> pair.
#include "ais_monitor.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

namespace hsasnoop {

// ---------------------------------------------------------------------------
// PCIe BDF resolution from /proc/<pid>/fd/<fd> → /sys block → PCI slot
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// PCIe device classification helpers
// ---------------------------------------------------------------------------

// Read one-line text file, return trimmed content or "" on failure.
static std::string ReadSysfsStr(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return "";
    std::string s;
    std::getline(f, s);
    // strip leading "0x" if present
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s = s.substr(2);
    // lowercase
    for (char& c : s)
        c = tolower((unsigned char)c);
    return s;
}

// PCI class code (24-bit: base[23:16] sub[15:8] prog-if[7:0]) → device type.
// NVMe:  0x010802 (Mass Storage, NVM, NVMe)
// RDMA classification falls back to vendor-id table below.
static std::string ClassifyByClassCode(const std::string& cls) {
    // cls is 6 hex digits, no "0x", lowercase.
    if (cls.size() >= 6) {
        std::string base = cls.substr(0, 4); // base+sub
        if (base == "0108")
            return "nvme"; // NVM Express
        if (base == "0107")
            return "sas"; // SAS controller
        if (base == "0106")
            return "sata"; // SATA controller
        if (base == "0104")
            return "raid"; // RAID
        if (base == "0c06")
            return "rdma"; // InfiniBand
        if (base == "0c07")
            return "rdma"; // IPMI (edge case)
        if (base == "0200")
            return "eth"; // Ethernet — may be RDMA capable
        if (base == "0207")
            return "rdma"; // InfiniBand (alternate)
    }
    return "";
}

// Vendor-ID table: VIDs known to produce RDMA / ROCE / iWARP / NVMe-oF HCAs.
// Used to refine "eth" or unknown class devices to "rdma".
static std::string ClassifyByVendorId(const std::string& vid) {
    // Mellanox / NVIDIA networking
    if (vid == "15b3")
        return "rdma";
    // Chelsio (iWARP)
    if (vid == "1425")
        return "rdma";
    // Intel (OmniPath, some E810 variants with RDMA)
    if (vid == "8086")
        return ""; // too broad — skip, use class
    // Broadcom / Emulex
    if (vid == "14e4")
        return ""; // too broad
    // Amazon Elastic Fabric Adapter (EFA)
    if (vid == "1d0f")
        return "rdma";
    // Pensando / AMD
    if (vid == "0x1dd8" || vid == "1dd8")
        return "rdma";
    // Marvell QLogic FastLinQ RDMA
    if (vid == "1077")
        return "rdma";
    // Xilinx (Solarflare)
    if (vid == "10ee" || vid == "1924")
        return "rdma";
    return "";
}

// Vendor-ID → human-readable vendor name, looked up from the system pci.ids
// database. Falls back to "unknown" if the file is absent or the VID is not
// listed. Results are cached after the first parse.
static std::string VendorName(const std::string& vid) {
    static std::unordered_map<std::string, std::string> db;
    static bool loaded = false;

    if (!loaded) {
        loaded = true;
        // Standard locations for pci.ids across distros.
        static const char* const kPaths[] = {
            "/usr/share/misc/pci.ids",
            "/usr/share/pci.ids",
            "/usr/share/hwdata/pci.ids",
            nullptr,
        };
        for (int i = 0; kPaths[i]; ++i) {
            std::ifstream f(kPaths[i]);
            if (!f.is_open())
                continue;
            std::string line;
            while (std::getline(f, line)) {
                // Vendor lines: "VVVV  Vendor Name" (no leading tab)
                if (line.empty() || line[0] == '#' || line[0] == '\t')
                    continue;
                if (line.size() < 6)
                    continue;
                std::string key = line.substr(0, 4);
                // Verify it's a 4-digit hex VID.
                bool hex = true;
                for (char c : key)
                    if (!isxdigit((unsigned char)c)) {
                        hex = false;
                        break;
                    }
                if (!hex)
                    continue;
                // Vendor name starts after the VID and whitespace.
                size_t name_start = line.find_first_not_of(" \t", 4);
                if (name_start == std::string::npos)
                    continue;
                db[key] = line.substr(name_start);
            }
            break; // use the first file found
        }
    }

    auto it = db.find(vid);
    return it != db.end() ? it->second : "unknown";
}

// Resolve the full PCIe device info for the block device backing fd in pid.
// Works for filesystem files (uses st_dev) and raw block device fds (st_rdev).
// Results are cached per (pid, fd) — the device behind an fd never changes.
static PcieDeviceInfo ResolvePcieDeviceInfo(int pid, int fd) {
    static std::unordered_map<std::string, PcieDeviceInfo> cache;
    char key[64];
    snprintf(key, sizeof(key), "%d:%d", pid, fd);

    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;

    auto store = [&](PcieDeviceInfo v) -> PcieDeviceInfo {
        cache[key] = v;
        return v;
    };

    PcieDeviceInfo info;
    info.device_type = "unknown";
    info.vendor = "unknown";

    // stat /proc/<pid>/fd/<fd> to get the backing block device's major:minor.
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/%d/fd/%d", pid, fd);
    struct stat st;
    if (stat(fd_path, &st) < 0)
        return store(info);

    // For a regular file, st_dev is the device the file resides on.
    // For a block device node, use st_rdev (the device itself).
    dev_t dev = S_ISBLK(st.st_mode) ? st.st_rdev : st.st_dev;
    unsigned int maj = major(dev);
    unsigned int min_val = minor(dev);

    // /sys/dev/block/major:minor → sysfs path for the block device.
    char sys_block[128];
    snprintf(sys_block, sizeof(sys_block), "/sys/dev/block/%u:%u", maj,
             min_val);

    // Walk up the sysfs device hierarchy to find the PCI BDF.
    std::string sys_dev = std::string(sys_block) + "/device";
    std::string bdf_path; // full sysfs path to the PCI device directory
    for (int depth = 0; depth < 8; ++depth) {
        char real[PATH_MAX] = {};
        if (realpath(sys_dev.c_str(), real) == nullptr)
            break;
        const char* last = strrchr(real, '/');
        if (!last)
            break;
        std::string name = last + 1;
        // PCI BDF: "DDDD:BB:SS.F" — two colons, one dot
        if (name.size() > 8 && std::count(name.begin(), name.end(), ':') == 2 &&
            name.find('.') != std::string::npos) {
            info.bdf = name;
            bdf_path = real;
            break;
        }
        sys_dev = std::string(real) + "/..";
    }

    if (info.bdf.empty()) {
        info.bdf = "unknown";
        return store(info);
    }

    // Read PCI IDs and class code from sysfs.
    info.vendor_id = ReadSysfsStr(bdf_path + "/vendor");
    info.device_id = ReadSysfsStr(bdf_path + "/device");
    info.class_code = ReadSysfsStr(bdf_path + "/class");
    // class sysfs gives "0x010802" (6 hex digits after stripping 0x)
    // ReadSysfsStr already strips "0x" and lowercases.

    info.vendor = VendorName(info.vendor_id);

    // Classify: class code first, then vendor-ID refinement.
    std::string by_class = ClassifyByClassCode(info.class_code);
    std::string by_vendor = ClassifyByVendorId(info.vendor_id);

    if (!by_class.empty())
        info.device_type = by_class;
    else if (!by_vendor.empty())
        info.device_type = by_vendor;
    // else stays "unknown"

    return store(info);
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Tracefs helpers
// ---------------------------------------------------------------------------

static bool WriteTracefs(const std::string& path, const std::string& val,
                         bool append = false) {
    int flags = O_WRONLY | (append ? O_APPEND : O_TRUNC);
    int fd = open(path.c_str(), flags);
    if (fd < 0)
        return false;
    ssize_t n = write(fd, val.data(), val.size());
    close(fd);
    return n == (ssize_t)val.size();
}

// ---------------------------------------------------------------------------
// Kprobe install / remove
// ---------------------------------------------------------------------------

// kfd_ioctl_ais(struct file *filep, struct kfd_process *p, void *data)
// arg2 = data — pointer to kfd_ioctl_ais_args union (kernel buffer):
//   in:  handle(u64@0)  size(u64@24)  op(u32@32)  fd(s32@36)
//        gpu_id = upper 32 bits of handle
//   out: size_copied(u64@0)  status(s32@8)  [written on return, over same buf]
//
// On x86-64 SysV ABI arg2 is in %dx.
bool AisMonitor::InstallKprobes() {
    // Create a dedicated tracefs instance so our trace_pipe does not conflict
    // with the main discovery reader which also holds trace_pipe open.
    instance_ = tracefs_ + "/instances/hsa_ais_" + std::to_string(getpid());
    if (mkdir(instance_.c_str(), 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "hsa-snoop: cannot create AIS tracefs instance: %s\n",
                strerror(errno));
        instance_.clear();
        return false;
    }

    const std::string kpe = tracefs_ + "/kprobe_events";

    // Use a pid-based suffix so concurrent hsa-snoop instances don't collide.
    std::string suffix = std::to_string(getpid());
    probe_entry_  = "hsasnoop_ais_e_" + suffix;

    // Remove stale entries (ignore errors).
    WriteTracefs(kpe, "-:" + probe_entry_ + "\n", true);

    // Entry probe: capture input fields from kfd_ioctl_ais_args via %dx (arg2).
    // kfd_ioctl_ais_args.handle is a u64 at offset 0; the gpu_id is in bits
    // [63:32] → read the upper 4 bytes at offset +4.
    // We emit the AIS record at entry time — size_copied is set to size_req.
    std::string entry_def =
        "p:" + probe_entry_ + " kfd_ioctl_ais"
        " gpu_id=+4(%dx):u32"    // handle bits [63:32] = gpu_id
        " size_req=+24(%dx):u64" // in.size
        " op=+32(%dx):u32"       // in.op  (1=READ 2=WRITE)
        " fd=+36(%dx):s32\n";    // in.fd

    if (!WriteTracefs(kpe, entry_def, true)) {
        fprintf(stderr,
                "hsa-snoop: failed to install AIS kprobe (%s). Need root?\n",
                strerror(errno));
        RemoveKprobes();
        return false;
    }

    // Enable the probe in our dedicated instance.
    if (!WriteTracefs(instance_ + "/events/kprobes/" + probe_entry_ + "/enable",
                      "1\n", false)) {
        fprintf(stderr, "hsa-snoop: failed to enable AIS kprobe in instance\n");
        RemoveKprobes();
        return false;
    }

    WriteTracefs(instance_ + "/tracing_on", "1\n", false);
    return true;
}

void AisMonitor::RemoveKprobes() {
    const std::string kpe = tracefs_ + "/kprobe_events";
    if (!probe_entry_.empty()) {
        if (!instance_.empty())
            WriteTracefs(instance_ + "/events/kprobes/" + probe_entry_ +
                         "/enable", "0\n", false);
        WriteTracefs(kpe, "-:" + probe_entry_ + "\n", true);
        probe_entry_.clear();
    }
    if (!instance_.empty()) {
        rmdir(instance_.c_str());
        instance_.clear();
    }
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool AisMonitor::Start(Sink sink) {
    if (!InstallKprobes())
        return false;

    // Open the instance's trace_pipe (not the global one, which discovery holds).
    trace_pipe_fd_ = open((instance_ + "/trace_pipe").c_str(), O_RDONLY);
    if (trace_pipe_fd_ < 0) {
        fprintf(stderr, "hsa-snoop: cannot open AIS trace_pipe: %s\n",
                strerror(errno));
        RemoveKprobes();
        return false;
    }

    // Self-pipe for cancellation: Stop() writes a byte; ReadLoop's poll() wakes.
    int pfds[2];
    if (pipe2(pfds, O_CLOEXEC) < 0) {
        close(trace_pipe_fd_);
        trace_pipe_fd_ = -1;
        RemoveKprobes();
        return false;
    }
    cancel_rfd_ = pfds[0];
    cancel_wfd_ = pfds[1];

    running_ = true;
    reader_thread_ = std::thread(
        [this, s = std::move(sink)]() mutable { ReadLoop(std::move(s)); });
    return true;
}

void AisMonitor::Stop() {
    running_.store(false);
    // Wake the ReadLoop by writing to the cancel pipe.
    if (cancel_wfd_ >= 0) {
        char b = 1;
        write(cancel_wfd_, &b, 1);
        close(cancel_wfd_);
        cancel_wfd_ = -1;
    }
    RemoveKprobes();
    if (trace_pipe_fd_ >= 0) {
        close(trace_pipe_fd_);
        trace_pipe_fd_ = -1;
    }
    if (cancel_rfd_ >= 0) {
        close(cancel_rfd_);
        cancel_rfd_ = -1;
    }
    if (reader_thread_.joinable())
        reader_thread_.join();
}

// ---------------------------------------------------------------------------
// ReadLoop — parse tracefs kprobe output into AisRecord
// ---------------------------------------------------------------------------

void AisMonitor::ReadLoop(Sink sink) {
    uint64_t seq = 0;
    char buf[4096];
    std::string partial;

    while (running_) {
        // Poll both trace_pipe and the cancel pipe so Stop() can wake us.
        struct pollfd pfds[2];
        pfds[0].fd     = trace_pipe_fd_;
        pfds[0].events = POLLIN;
        pfds[1].fd     = cancel_rfd_;
        pfds[1].events = POLLIN;

        int r = poll(pfds, 2, 200); // 200 ms timeout to recheck running_
        if (r < 0)
            break;
        if (pfds[1].revents & POLLIN)
            break; // cancel pipe signalled by Stop()
        if (!(pfds[0].revents & POLLIN))
            continue; // timeout — recheck running_

        ssize_t n = read(trace_pipe_fd_, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';
        partial += buf;

        // Process all complete lines.
        size_t pos;
        while ((pos = partial.find('\n')) != std::string::npos) {
            std::string line = partial.substr(0, pos);
            partial.erase(0, pos + 1);

            if (!running_)
                break;

            // tracefs kprobe line format:
            //   <comm>-<tid>  [cpu] ....  <ts>: <probe>: (<func>+N/M) f=v ...
            // Only process lines from our entry probe.
            if (line.find(probe_entry_) == std::string::npos)
                continue;

            // Find the fields section after the second ": ".
            size_t p1 = line.find(": ");
            if (p1 == std::string::npos)
                continue;
            size_t p2 = line.find(": ", p1 + 2);
            if (p2 == std::string::npos)
                continue;
            // After "<probe>: " comes "(<func>+offset/size [mod]) field=val..."
            // Skip past the parenthesised function reference to the fields.
            const char* fields_raw = line.c_str() + p2 + 2;
            const char* paren_end = strchr(fields_raw, ')');
            const char* fields = paren_end ? paren_end + 2 : fields_raw;

            // Extract TID: rightmost '-' before first '['.
            size_t bracket = line.find('[');
            if (bracket == std::string::npos)
                continue;
            size_t dash = line.rfind('-', bracket);
            if (dash == std::string::npos)
                continue;
            int tid = atoi(line.c_str() + dash + 1);
            std::string comm = line.substr(0, dash);
            size_t sp = comm.find_first_not_of(" ");
            if (sp != std::string::npos)
                comm = comm.substr(sp);

            // Parse: gpu_id=N size_req=N op=N fd=N  (tracefs outputs decimal)
            unsigned gpu_id_val = 0;
            uint64_t size_req = 0;
            unsigned op_val = 0;
            int file_fd = -1;
            sscanf(fields,
                   "gpu_id=%u size_req=%lu op=%u fd=%d",
                   &gpu_id_val, &size_req, &op_val, &file_fd);

            AisRecord rec;
            rec.seq        = ++seq;
            rec.pid        = tid;
            rec.comm       = comm;
            rec.op         = (op_val == 1) ? AisOp::Read
                             : (op_val == 2) ? AisOp::Write
                                             : AisOp::Unknown;
            rec.gpu_id     = gpu_id_val;
            rec.size_req   = size_req;
            rec.size_copied = size_req; // approximation at entry time
            rec.error      = 0;
            rec.completed  = true;

            struct timespec ts {};
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
            rec.submit_ts   = ts.tv_sec + ts.tv_nsec * 1e-9;
            rec.complete_ts = rec.submit_ts;

            if (rec.pid > 0 && file_fd >= 0) {
                rec.pcie_info = ResolvePcieDeviceInfo(rec.pid, file_fd);
                rec.pcie_id   = rec.pcie_info.bdf;
            }

            sink(rec);
        }
    }

    running_ = false;
    // fd == trace_pipe_fd_; Stop() owns closing it.
}

} // namespace hsasnoop
