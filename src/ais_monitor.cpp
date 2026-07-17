// ais_monitor.cpp - AIS IO monitor via bpftrace kprobe/kretprobe.
//
// Forks bpftrace with a script that probes kfd_ioctl_ais (the KFD ioctl
// handler for AMDKFD_IOC_AIS_OP). For each completed AIS ioctl the script
// prints a structured line; this file parses those lines and calls the sink.
//
// Output line format (space-separated, one per completed ioctl):
//   AIS pid=<n> comm=<s> op=<1|2> gpu_id=<n> size_req=<n> size_copied=<n>
//       fd=<n> err=<n> lat_ns=<n>
//
// The PCIe BDF string is resolved from <pid>'s /proc/PID/fdinfo/FD -> device
// -> /sys/class/block/... -> PCI slot once per unique <pid,fd> pair.
#include "ais_monitor.h"

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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
// bpftrace script
// ---------------------------------------------------------------------------

// Layout of kfd_ioctl_ais_args (union of in/out, kernel-space copy):
//   in:  handle(u64@0) handle_offset(u64@8) file_offset(s64@16) size(u64@24)
//        op(u32@32) fd(s32@36)
//   out: size_copied(u64@0) status(s32@8)
//
// At kretprobe time the output has been written over the same buffer, so
// offset 0 = size_copied (u64) and offset 8 = status (s32).
std::string AisMonitor::BuildBpftraceScript() {
    // The script uses per-tid maps to correlate entry and return probes.
    return R"(
#!/usr/bin/env bpftrace

// kfd_ioctl_ais(struct file *filep, struct kfd_process *p, void *data)
// arg0=filep  arg1=kfd_process*  arg2=data (kernel copy of kfd_ioctl_ais_args)
//
// kfd_ioctl_ais_args layout (as union in/out, kernel buffer):
//   in.handle       u64 @ offset  0  -> gpu_id = handle >> 32
//   in.handle_offset u64 @ offset  8
//   in.file_offset  s64 @ offset 16
//   in.size         u64 @ offset 24
//   in.op           u32 @ offset 32  (1=READ 2=WRITE)
//   in.fd           s32 @ offset 36
//   [on return the buffer is overwritten with out:]
//   out.size_copied u64 @ offset  0
//   out.status      s32 @ offset  8

BEGIN {
    printf("AIS_MONITOR_READY\n");
}

kprobe:kfd_ioctl_ais {
    $data  = (uint64)arg2;
    $handle   = *(uint64 *)$data;
    $size_req = *(uint64 *)($data + 24);
    $op       = *(uint32 *)($data + 32);
    $fd       = *(int32  *)($data + 36);
    $gpu_id   = (uint32)($handle >> 32);

    @ais_ts[tid]       = nsecs;
    @ais_data_ptr[tid] = $data;
    @ais_op[tid]       = $op;
    @ais_size_req[tid] = $size_req;
    @ais_gpu_id[tid]   = $gpu_id;
    @ais_fd[tid]       = (int64)$fd;
    @ais_pid[tid]      = (uint64)pid;
    @ais_comm[tid]     = comm;
}

kretprobe:kfd_ioctl_ais /@ais_ts[tid] != 0/ {
    $lat_ns     = nsecs - @ais_ts[tid];
    $data       = @ais_data_ptr[tid];
    $size_copied = *(uint64 *)$data;
    $err        = retval;
    $op         = @ais_op[tid];
    $size_req   = @ais_size_req[tid];
    $gpu_id     = @ais_gpu_id[tid];
    $fd         = @ais_fd[tid];
    $cpid       = @ais_pid[tid];
    $ccomm      = @ais_comm[tid];

    printf("AIS pid=%d comm=%s op=%d gpu_id=%d size_req=%llu size_copied=%llu fd=%d err=%d lat_ns=%llu\n",
           $cpid, $ccomm, $op, $gpu_id, $size_req, $size_copied, $fd, $err, $lat_ns);

    delete(@ais_ts[tid]);
    delete(@ais_data_ptr[tid]);
    delete(@ais_op[tid]);
    delete(@ais_size_req[tid]);
    delete(@ais_gpu_id[tid]);
    delete(@ais_fd[tid]);
    delete(@ais_pid[tid]);
    delete(@ais_comm[tid]);
}

END {
    clear(@ais_ts);
    clear(@ais_data_ptr);
    clear(@ais_op);
    clear(@ais_size_req);
    clear(@ais_gpu_id);
    clear(@ais_fd);
    clear(@ais_pid);
    clear(@ais_comm);
}
)";
}

// ---------------------------------------------------------------------------
// Start / Stop
// ---------------------------------------------------------------------------

bool AisMonitor::Start(Sink sink) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0)
        return false;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        // Child: run bpftrace. Redirect stdout to pipe.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        // Redirect stderr to /dev/null to suppress bpftrace noise.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        // Write the bpftrace script to a temp file and pass it as arg.
        // bpftrace does not support stdin scripts reliably across versions,
        // so we write to /tmp and exec with the path.
        char tmppath[] = "/tmp/ais-snoop-XXXXXX.bt";
        int tmpfd = mkstemps(tmppath, 3);
        if (tmpfd < 0)
            _exit(1);
        std::string script = BuildBpftraceScript();
        if (write(tmpfd, script.data(), script.size()) < 0) {
            close(tmpfd);
            _exit(1);
        }
        close(tmpfd);
        execlp("bpftrace", "bpftrace", tmppath, nullptr);
        // bpftrace not found or failed — clean up and exit.
        unlink(tmppath);
        _exit(127);
    }

    // Parent: close write end, keep read end.
    close(pipefd[1]);
    bpftrace_pid_ = pid;
    bpftrace_stdout_ = pipefd[0];

    running_ = true;
    reader_thread_ = std::thread(
        [this, s = std::move(sink)]() mutable { ReadLoop(std::move(s)); });
    return true;
}

void AisMonitor::Stop() {
    if (!running_.exchange(false))
        return;
    if (bpftrace_pid_ > 0) {
        kill(bpftrace_pid_, SIGTERM);
        // Give bpftrace a moment to flush END block output.
        usleep(200 * 1000);
        int status;
        waitpid(bpftrace_pid_, &status, WNOHANG);
        kill(bpftrace_pid_, SIGKILL);
        waitpid(bpftrace_pid_, nullptr, 0);
        bpftrace_pid_ = -1;
    }
    if (bpftrace_stdout_ >= 0) {
        close(bpftrace_stdout_);
        bpftrace_stdout_ = -1;
    }
    if (reader_thread_.joinable())
        reader_thread_.join();
}

// ---------------------------------------------------------------------------
// ReadLoop — parse bpftrace output lines into AisRecord
// ---------------------------------------------------------------------------

void AisMonitor::ReadLoop(Sink sink) {
    FILE* fp = fdopen(bpftrace_stdout_, "r");
    if (!fp) {
        running_ = false;
        return;
    }

    uint64_t seq = 0;
    char line[1024];
    bool ready = false;

    while (fgets(line, sizeof(line), fp)) {
        if (!running_)
            break;

        // Wait for the ready sentinel before processing events.
        if (!ready) {
            if (strstr(line, "AIS_MONITOR_READY"))
                ready = true;
            continue;
        }

        // Parse: AIS pid=<n> comm=<s> op=<n> gpu_id=<n> size_req=<n>
        //             size_copied=<n> fd=<n> err=<n> lat_ns=<n>
        if (strncmp(line, "AIS ", 4) != 0)
            continue;

        AisRecord rec;
        rec.seq = ++seq;

        char comm[256] = {};
        int op = 0, err = 0, fd = 0;
        uint64_t size_req = 0, size_copied = 0, lat_ns = 0;
        unsigned gpu_id = 0;

        int n = sscanf(line,
                       "AIS pid=%d comm=%255s op=%d gpu_id=%u "
                       "size_req=%lu size_copied=%lu fd=%d err=%d lat_ns=%lu",
                       &rec.pid, comm, &op, &gpu_id, &size_req, &size_copied,
                       &fd, &err, &lat_ns);
        if (n < 9)
            continue;

        rec.comm = comm;
        rec.op = (op == 1)   ? AisOp::Read
                 : (op == 2) ? AisOp::Write
                             : AisOp::Unknown;
        rec.gpu_id = gpu_id;
        rec.size_req = size_req;
        rec.size_copied = size_copied;
        rec.error = err;
        rec.completed = true;

        // Derive timestamps from lat_ns: place submit at (now - lat), complete
        // at now.
        struct timespec ts {};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        rec.complete_ts = ts.tv_sec + ts.tv_nsec * 1e-9;
        rec.submit_ts = rec.complete_ts - lat_ns * 1e-9;

        // Resolve PCIe device info from the file descriptor in the calling
        // process.
        if (rec.pid > 0 && fd >= 0) {
            rec.pcie_info = ResolvePcieDeviceInfo(rec.pid, fd);
            rec.pcie_id = rec.pcie_info.bdf;
        }

        sink(rec);
    }

    fclose(fp);
    bpftrace_stdout_ = -1;
    running_ = false;
}

} // namespace hsasnoop
