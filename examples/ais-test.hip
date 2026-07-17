// ais-test.hip - AMD Infinity Storage (AIS) P2P direct IO stress test.
//
// Exercises the AMDKFD_IOC_AIS_OP ioctl to drive direct P2P transfers between
// a file on an NVMe device and a VRAM buffer, generating AIS events visible to
// hsa-snoop --ais-snoop.
//
// The transfer path (file→VRAM or VRAM→file) uses PCIe P2P DMA, bypassing
// the CPU entirely. This requires a large-BAR dGPU and an NVMe device that
// both sit on the same PCIe root complex (or a CPU that supports P2P DMA
// across the fabric).
//
// Usage:
//   sudo ./ais-test [options]
//   ./ais-test --help
//
// AIS supports two IO modes:
//   - File on filesystem (e.g. /mnt/nvme/data.bin) — open with O_RDONLY/O_RDWR
//   - Raw block device  (e.g. /dev/nvme0n1)        — open with O_DIRECT
// Both modes work with this tool; the kernel's kfd_ais_rw_file() uses VFS IO
// internally, so any block-backed file or device node is valid. As AIS matures
// it will also support other PCIe-attached storage beyond NVMe (e.g. CXL,
// SCSI).
//
// Note: the current amdgpu DKMS AIS implementation (6.19) requires a file on
// a mounted filesystem — raw block device fds return -ENODEV because
// get_pci_dev_from_file() needs a VFS mount/superblock chain. Block device
// support is expected in a future AIS version.
//
// AIS ioctl flow:
//   1. Open /dev/kfd
//   2. AMDKFD_IOC_ALLOC_MEMORY_OF_GPU (VRAM) — allocate VRAM buffer
//   3. AMDKFD_IOC_MAP_MEMORY_TO_GPU — map VRAM buffer into GPU VA space
//   4. Open the target file or block device
//   5. AMDKFD_IOC_AIS_OP (READ / WRITE) — P2P transfer loop
//   6. AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU
//   7. AMDKFD_IOC_FREE_MEMORY_OF_GPU
//
// Build: compiled as part of the examples/ CMake target.
// No HIP runtime is used — GPU selection and gpu_id resolution are done
// entirely via /sys/class/kfd/kfd/topology so the tool starts without
// requiring an active ROCm context or any prior GPU workload.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// KFD UAPI definitions.
// The AIS ioctl (0x87) was added in amdgpu DKMS 6.19 and is not present in
// distro kernel headers. We include the system header for the base ioctls
// (ACQUIRE_VM, ALLOC/MAP/UNMAP/FREE MEMORY) and define AIS types inline.
// All definitions are guarded to avoid conflicts if a future distro header
// does include them.
#include <linux/kfd_ioctl.h>

#ifndef AMDKFD_IOC_AIS_OP
// AIS-specific types and ioctls — added in amdgpu 6.19, not yet in distro
// headers.
#define KFD_IOC_AIS_READ 1u
#define KFD_IOC_AIS_WRITE 2u

struct kfd_ais_in_args {
    uint64_t handle;
    uint64_t handle_offset;
    int64_t file_offset;
    uint64_t size;
    uint32_t op;
    int32_t fd;
};
struct kfd_ais_out_args {
    uint64_t size_copied;
    int32_t status;
    int32_t pad;
};
struct kfd_ioctl_ais_args {
    union {
        struct kfd_ais_in_args in;
        struct kfd_ais_out_args out;
    };
};
#define AMDKFD_IOC_AIS_OP AMDKFD_IOWR(0x87, struct kfd_ioctl_ais_args)
#endif // AMDKFD_IOC_AIS_OP

#define CHECK_IOCTL(fd, req, arg, label)                                       \
    do {                                                                       \
        if (ioctl((fd), (req), (arg)) < 0) {                                   \
            fprintf(stderr, "ioctl %s failed: %s\n", (label),                  \
                    strerror(errno));                                          \
            goto cleanup;                                                      \
        }                                                                      \
    } while (0)

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Enumerate KFD GPU devices from sysfs topology, ordered by node index.
// Fills gpu_id and opens the matching DRM render node (needed for ACQUIRE_VM).
// Returns true on success. No HIP runtime required.
//
// BDF derivation: topology/nodes/N/properties has "location_id <N>" where N
// encodes (domain<<20)|(bus<<8)|(slot<<3)|fn on Genoa/modern AMD platforms.
// We then find the render node via /sys/bus/pci/devices/<BDF>/drm/renderD*.
static bool find_kfd_device(int device_index, uint32_t* gpu_id_out,
                             int* render_fd_out) {
    int found = 0;
    for (int n = 0; n < 128; ++n) {
        char path[256];
        snprintf(path, sizeof(path),
                 "/sys/class/kfd/kfd/topology/nodes/%d/gpu_id", n);
        FILE* f = fopen(path, "r");
        if (!f)
            continue;
        uint32_t id = 0;
        int ok = (fscanf(f, "%u", &id) == 1 && id != 0);
        fclose(f);
        if (!ok)
            continue;
        if (found++ != device_index)
            continue;

        *gpu_id_out = id;
        *render_fd_out = -1;

        // Read location_id from properties to decode the PCI BDF.
        snprintf(path, sizeof(path),
                 "/sys/class/kfd/kfd/topology/nodes/%d/properties", n);
        f = fopen(path, "r");
        if (!f)
            return true;
        uint32_t location_id = 0;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "location_id %u", &location_id) == 1)
                break;
        }
        fclose(f);
        if (!location_id)
            return true;

        // Decode location_id → BDF (AMD: domain=0, bus=bits[15:8]).
        unsigned bus  = (location_id >> 8) & 0xff;
        unsigned slot = (location_id >> 3) & 0x1f;
        unsigned fn   = location_id & 0x7;
        char bdf[32];
        snprintf(bdf, sizeof(bdf), "0000:%02x:%02x.%u", bus, slot, fn);
        fprintf(stderr, "ais-test: GPU device=%d gpu_id=%u bdf=%s\n",
                device_index, id, bdf);

        // Find renderD* under /sys/bus/pci/devices/<BDF>/drm/.
        char drm_dir[256];
        snprintf(drm_dir, sizeof(drm_dir), "/sys/bus/pci/devices/%s/drm", bdf);
        DIR* dir = opendir(drm_dir);
        if (!dir)
            return true;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            int r = -1;
            if (sscanf(ent->d_name, "renderD%d", &r) == 1 && r >= 128) {
                char dev_path[64];
                snprintf(dev_path, sizeof(dev_path), "/dev/dri/renderD%d", r);
                *render_fd_out = open(dev_path, O_RDWR | O_CLOEXEC);
                fprintf(stderr, "ais-test: render node: %s\n", dev_path);
                break;
            }
        }
        closedir(dir);
        return true;
    }
    return false;
}

static void usage(const char* prog) {
    printf(
        "Usage: %s [options] <file>\n"
        "\n"
        "AIS (AMD Infinity Storage) P2P direct IO test.\n"
        "Transfers data between a file on NVMe and GPU VRAM via PCIe P2P,\n"
        "generating AIS ioctl events visible to hsa-snoop --ais-snoop.\n"
        "\n"
        "  <target>         Path to a file on an NVMe-backed filesystem OR a\n"
        "                   raw block device (e.g. /dev/nvme0n1).\n"
        "                   Note: the current amdgpu DKMS AIS implementation\n"
        "                   requires a file on a mounted filesystem; raw "
        "block\n"
        "                   devices return -ENODEV (block device support is\n"
        "                   planned for a future AIS version).\n"
        "\n"
        "Options:\n"
        "  -h, --help         Show this help and exit\n"
        "  -d, --device N     GPU device index (0-based, default 0)\n"
        "  -s, --size N       IO size in KiB per transfer (default 64).\n"
        "                     0 = random size each iteration, drawn uniformly\n"
        "                     from power-of-two values between 4 KiB and 8 "
        "MiB.\n"
        "  -D, --duration N   Run for N seconds then stop (default 30).\n"
        "                     -1 = run indefinitely until Ctrl-C.\n"
        "  -i, --iters N      Stop after N iterations regardless of duration.\n"
        "                     0 = no iteration limit (default; duration "
        "governs).\n"
        "  -w, --write        Transfer direction: VRAM→file (default: "
        "file→VRAM)\n"
        "  -r, --read         Transfer direction: file→VRAM (default)\n"
        "  -b, --both         Run both read and write in alternating "
        "iterations\n"
        "  -v, --verbose      Print one line per IO (default: rolling stats "
        "line)\n"
        "\n"
        "Requirements:\n"
        "  - Large-BAR dGPU (visible_vram == real_vram)\n"
        "  - NVMe device on the same PCIe root complex\n"
        "  - amdgpu DKMS >= 6.19 with AIS support (CONFIG_PCI_P2PDMA=y)\n"
        "  - Must run as root (or with CAP_SYS_ADMIN) for KFD alloc\n",
        prog);
}

typedef enum { DIR_READ = 0, DIR_WRITE = 1, DIR_BOTH = 2 } TransferDir;

int main(int argc, char* argv[]) {
    int device_index = 0;
    int size_kib = 64;     // 0 = random per-iteration
    int duration_sec = 30; // -1 = indefinite
    int max_iters = 0;     // 0 = no iteration cap
    bool verbose = false;
    TransferDir dir = DIR_READ;
    const char* filepath = NULL;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--device")) &&
                   i + 1 < argc) {
            device_index = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--size")) &&
                   i + 1 < argc) {
            size_kib = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-D") || !strcmp(argv[i], "--duration")) &&
                   i + 1 < argc) {
            duration_sec = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--iters")) &&
                   i + 1 < argc) {
            max_iters = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "-w") || !strcmp(argv[i], "--write")) {
            dir = DIR_WRITE;
        } else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--read")) {
            dir = DIR_READ;
        } else if (!strcmp(argv[i], "-b") || !strcmp(argv[i], "--both")) {
            dir = DIR_BOTH;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = true;
        } else if (argv[i][0] != '-') {
            filepath = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    if (!filepath) {
        fprintf(stderr,
                "ais-test: specify a file path on an NVMe filesystem\n\n");
        usage(argv[0]);
        return 2;
    }
    if (size_kib < 0) {
        fprintf(stderr, "ais-test: --size must be >= 0\n");
        return 2;
    }

    const long page_size = sysconf(_SC_PAGESIZE);
    const bool random_size = (size_kib == 0);

    // Power-of-two sizes for random mode: 4 KiB .. 8 MiB (12 choices).
    // AIS requires PAGE_ALIGNED sizes; all of these are multiples of 4 KiB.
    static const size_t kRandSizes[] = {
        4096,   8192,   16384,   32768,   65536,   131072,
        262144, 524288, 1048576, 2097152, 4194304, 8388608,
    };
    const int kNumRandSizes = (int)(sizeof(kRandSizes) / sizeof(kRandSizes[0]));

    // Maximum IO size determines the VRAM buffer allocation.
    size_t max_io_size = random_size ? kRandSizes[kNumRandSizes - 1] // 8 MiB
                                     : (size_t)size_kib * 1024;
    max_io_size = (max_io_size + page_size - 1) & ~(size_t)(page_size - 1);

    if (!random_size && max_io_size == 0) {
        fprintf(stderr,
                "ais-test: --size must be > 0 KiB or exactly 0 for random\n");
        return 2;
    }

    {
        char size_str[32];
        if (random_size)
            snprintf(size_str, sizeof(size_str), "random(4KiB-8MiB)");
        else if (size_kib >= 1024 && size_kib % 1024 == 0)
            snprintf(size_str, sizeof(size_str), "%d MiB", size_kib / 1024);
        else
            snprintf(size_str, sizeof(size_str), "%d KiB", size_kib);

        char dur_str[32];
        if (duration_sec < 0)
            snprintf(dur_str, sizeof(dur_str), "indefinite");
        else
            snprintf(dur_str, sizeof(dur_str), "%ds", duration_sec);

        printf("ais-test: device=%d target=%s size=%s duration=%s dir=%s\n",
               device_index, filepath, size_str, dur_str,
               dir == DIR_READ    ? "read(file→VRAM)"
               : dir == DIR_WRITE ? "write(VRAM→file)"
                                  : "both");
        if (max_iters)
            printf("ais-test: stopping after %d iterations\n", max_iters);
    }
    fflush(stdout);

    // Resolve GPU device via KFD sysfs topology — no HIP runtime needed.
    uint32_t gpu_id = 0;
    int render_fd = -1;
    if (!find_kfd_device(device_index, &gpu_id, &render_fd)) {
        fprintf(stderr,
                "ais-test: no KFD GPU at device index %d "
                "(check /sys/class/kfd/kfd/topology/nodes/)\n",
                device_index);
        return 1;
    }

    int kfd_fd = -1;
    int file_fd = -1;
    uint64_t mem_handle = 0;
    uint64_t va_addr = 0;
    uint64_t target_size = 0;
    bool mapped = false;
    int ret = 1;

    kfd_fd = open("/dev/kfd", O_RDWR | O_CLOEXEC);
    if (kfd_fd < 0) {
        perror("ais-test: open /dev/kfd");
        return 1;
    }

    // ACQUIRE_VM binds this process to the GPU VM address space.
    // Required before ALLOC_MEMORY_OF_GPU; pass the render node fd.
    if (render_fd < 0) {
        fprintf(stderr,
                "ais-test: could not open render node for gpu_id=%u; "
                "ALLOC_MEMORY_OF_GPU will fail\n",
                gpu_id);
        goto cleanup;
    }
    {
        struct kfd_ioctl_acquire_vm_args avm = {};
        avm.drm_fd = (uint32_t)render_fd;
        avm.gpu_id = gpu_id;
        if (ioctl(kfd_fd, AMDKFD_IOC_ACQUIRE_VM, &avm) < 0 &&
            errno != EBUSY) {
            fprintf(stderr, "ais-test: ACQUIRE_VM failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }
        close(render_fd);
        render_fd = -1;
    }
    if (gpu_id == 0) {
        fprintf(stderr, "ais-test: could not resolve KFD gpu_id\n");
        goto cleanup;
    }
    fprintf(stderr, "ais-test: KFD gpu_id=%u\n", gpu_id);

    // Note: ACQUIRE_VM is intentionally skipped here. The HIP runtime already
    // called it when the context was initialised above (hipFree(nullptr)).
    // A second ACQUIRE_VM on the same GPU returns EBUSY.

    // Allocate a VRAM buffer sized to the maximum IO (8 MiB for random mode,
    // otherwise the fixed IO size). VA hint 0x700000000000; KFD may adjust.
    // Public+Writable so the BO is accessible for P2P.
    va_addr = 0x700000000000ULL;
    {
        struct kfd_ioctl_alloc_memory_of_gpu_args alloc = {};
        alloc.va_addr = va_addr;
        alloc.size = max_io_size;
        alloc.gpu_id = gpu_id;
        alloc.flags = KFD_IOC_ALLOC_MEM_FLAGS_VRAM |
                      KFD_IOC_ALLOC_MEM_FLAGS_WRITABLE |
                      KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC;
        if (ioctl(kfd_fd, AMDKFD_IOC_ALLOC_MEMORY_OF_GPU, &alloc) < 0) {
            fprintf(stderr,
                    "ais-test: ALLOC_MEMORY_OF_GPU failed: %s\n"
                    "  (Is AIS initialized on this GPU? Large-BAR required; "
                    "check dmesg for 'AIS: registered')\n",
                    strerror(errno));
            goto cleanup;
        }
        mem_handle = alloc.handle;
        va_addr = alloc.va_addr; // KFD may have adjusted the VA
    }
    fprintf(stderr,
            "ais-test: allocated %zu KiB VRAM handle=0x%llx va=0x%llx\n",
            max_io_size / 1024, (unsigned long long)mem_handle,
            (unsigned long long)va_addr);

    // Map the VRAM buffer into GPU VA space.
    {
        struct kfd_ioctl_map_memory_to_gpu_args map = {};
        map.handle = mem_handle;
        map.device_ids_array_ptr = (uint64_t)(uintptr_t)&gpu_id;
        map.n_devices = 1;
        map.n_success = 0;
        CHECK_IOCTL(kfd_fd, AMDKFD_IOC_MAP_MEMORY_TO_GPU, &map,
                    "MAP_MEMORY_TO_GPU");
        mapped = true;
    }

    // Open the target — either a filesystem file or a raw block device.
    // Block devices always use O_DIRECT; files try O_DIRECT first then fall
    // back (some filesystems don't support it).
    {
        struct stat st;
        bool is_block = (stat(filepath, &st) == 0 && S_ISBLK(st.st_mode));

        int flags = (dir == DIR_READ) ? O_RDONLY : O_RDWR;
        flags |= O_CLOEXEC;

        file_fd = open(filepath, flags | O_DIRECT);
        if (file_fd < 0 && !is_block)
            file_fd = open(filepath, flags); // retry without O_DIRECT
        if (file_fd < 0) {
            fprintf(stderr, "ais-test: open %s: %s\n", filepath,
                    strerror(errno));
            goto cleanup;
        }

        // Determine target size for offset clamping.
        if (is_block) {
            // BLKGETSIZE64 returns the block device size in bytes.
            if (ioctl(file_fd, BLKGETSIZE64, &target_size) < 0) {
                fprintf(stderr, "ais-test: BLKGETSIZE64 %s: %s\n", filepath,
                        strerror(errno));
                goto cleanup;
            }
        } else {
            struct stat fst;
            if (fstat(file_fd, &fst) < 0) {
                fprintf(stderr, "ais-test: fstat %s: %s\n", filepath,
                        strerror(errno));
                goto cleanup;
            }
            target_size = (uint64_t)fst.st_size;
            // For write tests ensure the file covers the maximum IO window.
            if (dir != DIR_READ && target_size < max_io_size) {
                if (ftruncate(file_fd, (off_t)max_io_size) < 0) {
                    fprintf(stderr, "ais-test: ftruncate %s: %s\n", filepath,
                            strerror(errno));
                    goto cleanup;
                }
                target_size = max_io_size;
            }
        }
        fprintf(stderr, "ais-test: opened %s (%s, %.2f GiB)\n", filepath,
                is_block ? "block device" : "file",
                target_size / (double)(1ULL << 30));
    }

    if (target_size < max_io_size) {
        fprintf(stderr,
                "ais-test: target too small (%llu B) for IO size (%zu B)\n",
                (unsigned long long)target_size, max_io_size);
        goto cleanup;
    }

    // Main transfer loop.
    {
        uint64_t total_rx_bytes = 0, total_tx_bytes = 0;
        int rx_ops = 0, tx_ops = 0, errors = 0;
        int iter = 0;
        double t0 = now_sec();
        double deadline = (duration_sec >= 0) ? t0 + duration_sec : -1.0;

        // Seed the random state from the kernel.
        uint64_t rng_state = 0;
        if (getrandom(&rng_state, sizeof(rng_state), 0) < 0)
            rng_state = (uint64_t)(uintptr_t)&rng_state ^ (uint64_t)now_sec();

        // xorshift64 — fast, good enough for random offsets.
        auto rng_next = [&]() -> uint64_t {
            rng_state ^= rng_state << 13;
            rng_state ^= rng_state >> 7;
            rng_state ^= rng_state << 17;
            return rng_state;
        };

        for (;;) {
            // Duration check.
            if (deadline >= 0 && now_sec() >= deadline)
                break;
            // Iteration cap.
            if (max_iters > 0 && iter >= max_iters)
                break;

            // Pick IO size for this iteration.
            size_t io_size;
            if (random_size) {
                int idx = (int)(rng_next() % (uint64_t)kNumRandSizes);
                io_size = kRandSizes[idx];
            } else {
                io_size = max_io_size;
            }

            // Random page-aligned file offset within [0, target_size -
            // io_size].
            uint64_t offset_range = target_size - io_size;
            // Round down to page boundary.
            offset_range =
                (offset_range / (uint64_t)page_size) * (uint64_t)page_size;
            int64_t file_offset = 0;
            if (offset_range > 0)
                file_offset = (int64_t)((rng_next() %
                                         (offset_range / (uint64_t)page_size)) *
                                        (uint64_t)page_size);

            uint32_t op;
            const char* dir_label;
            if (dir == DIR_READ || (dir == DIR_BOTH && (iter % 2) == 0)) {
                op = KFD_IOC_AIS_READ;
                dir_label = "READ (file→VRAM)";
            } else {
                op = KFD_IOC_AIS_WRITE;
                dir_label = "WRITE(VRAM→file)";
            }

            struct kfd_ioctl_ais_args ais = {};
            ais.in.handle = mem_handle;
            ais.in.handle_offset = 0;
            ais.in.file_offset = file_offset;
            ais.in.size = io_size;
            ais.in.op = op;
            ais.in.fd = file_fd;

            double t_start = now_sec();
            int rc = ioctl(kfd_fd, AMDKFD_IOC_AIS_OP, &ais);
            double lat = now_sec() - t_start;
            ++iter;

            if (rc < 0 || ais.out.status != 0) {
                int err = (rc < 0) ? errno : -ais.out.status;
                fprintf(stderr,
                        "  iter %4d %-20s off=0x%llx size=%zuKiB FAILED "
                        "errno=%d (%s)\n",
                        iter, dir_label, (unsigned long long)file_offset,
                        io_size / 1024, err, strerror(err));
                ++errors;
                if (err == ENODEV) {
                    fprintf(stderr,
                            "  Hint: -ENODEV means AIS is not initialized.\n"
                            "  Check: dmesg | grep 'AIS:'\n"
                            "  Requires: large-BAR dGPU + P2P-capable NVMe\n");
                    break; // no point continuing
                }
                continue;
            }

            uint64_t copied = ais.out.size_copied;
            double bw_gbs = (lat > 0) ? (copied / 1e9) / lat : 0.0;

            if (op == KFD_IOC_AIS_READ) {
                total_rx_bytes += copied;
                ++rx_ops;
            } else {
                total_tx_bytes += copied;
                ++tx_ops;
            }

            if (verbose) {
                printf("  iter %4d %-20s off=0x%010llx size=%4zuKiB %5.1f ms "
                       "%6.2f GB/s\n",
                       iter, dir_label, (unsigned long long)file_offset,
                       io_size / 1024, lat * 1e3, bw_gbs);
                fflush(stdout);
            } else {
                // Rolling stats line: overwrite in place once per second.
                double elapsed_now = now_sec() - t0;
                double rx_bw = elapsed_now > 0
                                   ? (total_rx_bytes / 1e9) / elapsed_now
                                   : 0.0;
                double tx_bw = elapsed_now > 0
                                   ? (total_tx_bytes / 1e9) / elapsed_now
                                   : 0.0;
                printf("\r  ops=%d rx=%d(%.2fGB/s) tx=%d(%.2fGB/s) err=%d "
                       "elapsed=%.0fs   ",
                       iter, rx_ops, rx_bw, tx_ops, tx_bw, errors, elapsed_now);
                fflush(stdout);
            }
        }

        double elapsed = now_sec() - t0;
        if (!verbose)
            printf("\n"); // end the rolling stats line
        printf("\nSummary: %d iters in %.2fs\n", iter, elapsed);
        if (rx_ops)
            printf("  RX (file→VRAM): %d ops, %.3f GB total, %.2f GB/s avg\n",
                   rx_ops, total_rx_bytes / 1e9,
                   elapsed > 0 ? (total_rx_bytes / 1e9) / elapsed : 0.0);
        if (tx_ops)
            printf("  TX (VRAM→file): %d ops, %.3f GB total, %.2f GB/s avg\n",
                   tx_ops, total_tx_bytes / 1e9,
                   elapsed > 0 ? (total_tx_bytes / 1e9) / elapsed : 0.0);
        if (errors)
            printf("  Errors: %d\n", errors);
    }
    ret = 0;

cleanup:
    if (file_fd >= 0)
        close(file_fd);
    if (render_fd >= 0)
        close(render_fd);
    if (mapped && mem_handle) {
        struct kfd_ioctl_unmap_memory_from_gpu_args unmap = {};
        unmap.handle = mem_handle;
        unmap.device_ids_array_ptr = (uint64_t)(uintptr_t)&gpu_id;
        unmap.n_devices = 1;
        ioctl(kfd_fd, AMDKFD_IOC_UNMAP_MEMORY_FROM_GPU, &unmap);
    }
    if (mem_handle) {
        struct kfd_ioctl_free_memory_of_gpu_args fr = {};
        fr.handle = mem_handle;
        ioctl(kfd_fd, AMDKFD_IOC_FREE_MEMORY_OF_GPU, &fr);
    }
    if (kfd_fd >= 0)
        close(kfd_fd);
    return ret;
}
