#include <hip/hip_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        hipError_t e = (x);                                                    \
        if (e != hipSuccess) {                                                 \
            fprintf(stderr, "HIP error %s:%d: %s\n", __FILE__, __LINE__,       \
                    hipGetErrorString(e));                                     \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void usage(const char* prog) {
    printf(
        "Usage: %s [options]\n"
        "\n"
        "SDMA copy stress test: drives hipMemcpyAsync across multiple streams\n"
        "to generate H2D, D2D, and D2H traffic visible to hsa-snoop.\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help and exit\n"
        "  -s, --streams N     Number of async streams (default 4)\n"
        "  -m, --buf-mb N      Per-stream buffer size in MB (default 256)\n"
        "  -i, --iters N       Number of copy iterations (default 40)\n"
        "  -r, --report N      Print throughput every N iterations (default "
        "10)\n",
        prog);
}

int main(int argc, char* argv[]) {
    int nstreams = 4;
    int buf_mb = 256;
    int iters = 40;
    int report_every = 10;

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        } else if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--streams")) &&
                   i + 1 < argc) {
            nstreams = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-m") || !strcmp(argv[i], "--buf-mb")) &&
                   i + 1 < argc) {
            buf_mb = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-i") || !strcmp(argv[i], "--iters")) &&
                   i + 1 < argc) {
            iters = atoi(argv[++i]);
        } else if ((!strcmp(argv[i], "-r") || !strcmp(argv[i], "--report")) &&
                   i + 1 < argc) {
            report_every = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    size_t n = (size_t)buf_mb * 1024 * 1024;

    printf("sdma-test: %d streams x %d MB x %d iters (H2D + D2D + D2H)\n",
           nstreams, buf_mb, iters);
    fflush(stdout);

    float** h_src = (float**)malloc(nstreams * sizeof(float*));
    float** h_dst = (float**)malloc(nstreams * sizeof(float*));
    float** d_a = (float**)malloc(nstreams * sizeof(float*));
    float** d_b = (float**)malloc(nstreams * sizeof(float*));
    hipStream_t* streams = (hipStream_t*)malloc(nstreams * sizeof(hipStream_t));

    for (int s = 0; s < nstreams; s++) {
        CHECK(hipHostMalloc(&h_src[s], n, hipHostMallocDefault));
        CHECK(hipHostMalloc(&h_dst[s], n, hipHostMallocDefault));
        memset(h_src[s], 0x42, n);
        CHECK(hipMalloc(&d_a[s], n));
        CHECK(hipMalloc(&d_b[s], n));
        CHECK(hipStreamCreate(&streams[s]));
        // Warm up: fault pages in before timing starts
        CHECK(hipMemcpyAsync(d_a[s], h_src[s], n, hipMemcpyHostToDevice,
                             streams[s]));
    }
    CHECK(hipDeviceSynchronize());

    double t0 = now_sec();
    uint64_t total_bytes = 0;

    for (int i = 0; i < iters; i++) {
        for (int s = 0; s < nstreams; s++) {
            CHECK(hipMemcpyAsync(d_a[s], h_src[s], n, hipMemcpyHostToDevice,
                                 streams[s]));
            CHECK(hipMemcpyAsync(d_b[s], d_a[s], n, hipMemcpyDeviceToDevice,
                                 streams[s]));
            CHECK(hipMemcpyAsync(h_dst[s], d_b[s], n, hipMemcpyDeviceToHost,
                                 streams[s]));
            total_bytes += 3 * n;
        }
        CHECK(hipDeviceSynchronize());

        if (report_every > 0 && (i + 1) % report_every == 0) {
            double elapsed = now_sec() - t0;
            double gb = total_bytes / 1e9;
            printf("  iter %3d: %.2f GB moved, %.2f GB/s\n", i + 1, gb,
                   gb / elapsed);
            fflush(stdout);
        }
    }

    double elapsed = now_sec() - t0;
    double gb = total_bytes / 1e9;
    printf("Done: %.2f GB in %.2fs = %.2f GB/s aggregate\n", gb, elapsed,
           gb / elapsed);

    for (int s = 0; s < nstreams; s++) {
        (void)hipStreamDestroy(streams[s]);
        (void)hipFree(d_a[s]);
        (void)hipFree(d_b[s]);
        (void)hipHostFree(h_src[s]);
        (void)hipHostFree(h_dst[s]);
    }
    free(streams);
    free(d_a);
    free(d_b);
    free(h_src);
    free(h_dst);
    return 0;
}
