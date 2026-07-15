#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#define CHECK(x) do { \
    hipError_t e = (x); \
    if (e != hipSuccess) { \
        fprintf(stderr, "HIP error %s:%d: %s\n", __FILE__, __LINE__, hipGetErrorString(e)); \
        exit(1); \
    } \
} while(0)

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char* argv[]) {
    int nstreams = argc > 1 ? atoi(argv[1]) : 4;
    int buf_mb   = argc > 2 ? atoi(argv[2]) : 256;
    int iters    = argc > 3 ? atoi(argv[3]) : 40;

    size_t n = (size_t)buf_mb * 1024 * 1024;

    printf("SDMA stress: %d async streams x %d MB x %d iters (H2D + D2D + D2H)\n",
           nstreams, buf_mb, iters);
    fflush(stdout);

    // Allocate per-stream buffers
    float** h_src = (float**)malloc(nstreams * sizeof(float*));
    float** h_dst = (float**)malloc(nstreams * sizeof(float*));
    float** d_a   = (float**)malloc(nstreams * sizeof(float*));
    float** d_b   = (float**)malloc(nstreams * sizeof(float*));
    hipStream_t* streams = (hipStream_t*)malloc(nstreams * sizeof(hipStream_t));

    for (int s = 0; s < nstreams; s++) {
        CHECK(hipHostMalloc(&h_src[s], n, hipHostMallocDefault));
        CHECK(hipHostMalloc(&h_dst[s], n, hipHostMallocDefault));
        memset(h_src[s], 0x42, n);
        CHECK(hipMalloc(&d_a[s], n));
        CHECK(hipMalloc(&d_b[s], n));
        CHECK(hipStreamCreate(&streams[s]));
        // Warm up: get pages faulted in
        CHECK(hipMemcpyAsync(d_a[s], h_src[s], n, hipMemcpyHostToDevice, streams[s]));
    }
    CHECK(hipDeviceSynchronize());

    double t0 = now_sec();
    uint64_t total_bytes = 0;

    for (int i = 0; i < iters; i++) {
        // Issue all copies on all streams without waiting between streams
        for (int s = 0; s < nstreams; s++) {
            CHECK(hipMemcpyAsync(d_a[s], h_src[s], n, hipMemcpyHostToDevice,  streams[s]));
            CHECK(hipMemcpyAsync(d_b[s], d_a[s],   n, hipMemcpyDeviceToDevice, streams[s]));
            CHECK(hipMemcpyAsync(h_dst[s], d_b[s], n, hipMemcpyDeviceToHost,  streams[s]));
            total_bytes += 3 * n;
        }
        CHECK(hipDeviceSynchronize());

        if ((i + 1) % 10 == 0) {
            double elapsed = now_sec() - t0;
            double gb = total_bytes / 1e9;
            printf("  iter %3d: %.2f GB moved, %.2f GB/s\n",
                   i + 1, gb, gb / elapsed);
            fflush(stdout);
        }
    }

    double elapsed = now_sec() - t0;
    double gb = total_bytes / 1e9;
    printf("Done: %.2f GB in %.2fs = %.2f GB/s aggregate\n",
           gb, elapsed, gb / elapsed);

    for (int s = 0; s < nstreams; s++) {
        (void)hipStreamDestroy(streams[s]);
        (void)hipFree(d_a[s]); (void)hipFree(d_b[s]);
        (void)hipHostFree(h_src[s]); (void)hipHostFree(h_dst[s]);
    }
    free(streams); free(d_a); free(d_b); free(h_src); free(h_dst);
    return 0;
}
