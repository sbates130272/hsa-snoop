#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <hip/hip_runtime.h>
#include <iostream>
#include <unistd.h>

#define HIP_CHECK(call)                                                        \
    do {                                                                       \
        hipError_t err = (call);                                               \
        if (err != hipSuccess) {                                               \
            std::cerr << "HIP error: " << hipGetErrorString(err) << " at "    \
                      << __FILE__ << ":" << __LINE__ << std::endl;            \
            std::exit(EXIT_FAILURE);                                           \
        }                                                                      \
    } while (0)

__global__ void vadd(const float* a, const float* b, float* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        c[i] = a[i] + b[i];
}

__global__ void vscale(float* a, float s, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        a[i] *= s;
}

// Compute-heavy kernel: accumulates a dot product in shared memory.
// The loop count keeps the GPU busy for a meaningful duration per launch.
__global__ void vdot(const float* a, const float* b, float* result, int n,
                     int iters) {
    __shared__ float sdata[256];
    int i   = blockIdx.x * blockDim.x + threadIdx.x;
    float v = 0.0f;
    for (int it = 0; it < iters; ++it)
        if (i < n)
            v += a[i] * b[i];
    sdata[threadIdx.x] = v;
    __syncthreads();
    if (threadIdx.x == 0) {
        float s = 0.0f;
        for (int t = 0; t < 256; ++t)
            s += sdata[t];
        atomicAdd(result, s);
    }
}

static void usage(const char* prog) {
    fprintf(stderr,
            "Usage: %s [--iters N] [--batch N] [--sleep-ms N] [--loops N]\n"
            "  --iters    N  Inner loop count inside vdot kernel (default 64)\n"
            "  --batch    N  Kernel launches per outer loop iteration (default 10)\n"
            "  --sleep-ms N  Sleep between outer iterations in ms (default 200)\n"
            "  --loops    N  Number of outer iterations; 0 = run forever (default 0)\n",
            prog);
}

int main(int argc, char** argv) {
    int iters    = 1024;
    int batch    = 10;
    int sleep_ms = 200;
    int loops    = 0;  // 0 = infinite

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iters") && i + 1 < argc)
            iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--batch") && i + 1 < argc)
            batch = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sleep-ms") && i + 1 < argc)
            sleep_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
            loops = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    const int n = 1 << 24;  // 16M floats
    const size_t sz = (size_t)n * sizeof(float);

    float *a, *b, *c, *result;
    HIP_CHECK(hipMalloc(&a,      sz));
    HIP_CHECK(hipMalloc(&b,      sz));
    HIP_CHECK(hipMalloc(&c,      sz));
    HIP_CHECK(hipMalloc(&result, sizeof(float)));

    // Initialise on device.
    HIP_CHECK(hipMemset(a, 0x3f, sz));  // ~0.5f per element
    HIP_CHECK(hipMemset(b, 0x3f, sz));
    HIP_CHECK(hipMemset(result, 0, sizeof(float)));

    const dim3 block(256);
    const dim3 grid((n + 255) / 256);

    fprintf(stderr,
            "vecwork: n=%d iters=%d batch=%d sleep_ms=%d loops=%s\n",
            n, iters, batch, sleep_ms,
            loops == 0 ? "infinite" : std::to_string(loops).c_str());

    long long total = 0;
    for (int outer = 0; loops == 0 || outer < loops; ++outer) {
        for (int k = 0; k < batch; ++k) {
            hipLaunchKernelGGL(vadd,   grid, block, 0, 0, a, b, c, n);
            hipLaunchKernelGGL(vscale, grid, block, 0, 0, c, 0.5f, n);
            hipLaunchKernelGGL(vdot,   grid, block, 0, 0, a, b, result, n, iters);
        }
        HIP_CHECK(hipDeviceSynchronize());
        total += batch * 3;
        fprintf(stderr, "vecwork: outer=%d total_launches=%lld\n", outer, total);
        if (sleep_ms > 0)
            usleep((unsigned)(sleep_ms * 1000));
    }

    HIP_CHECK(hipFree(a));
    HIP_CHECK(hipFree(b));
    HIP_CHECK(hipFree(c));
    HIP_CHECK(hipFree(result));
    fprintf(stderr, "vecwork: done, total_launches=%lld\n", total);
    return 0;
}
