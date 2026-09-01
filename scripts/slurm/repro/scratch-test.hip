#include <cstdio>
#include <hip/hip_runtime.h>
#include <unistd.h>

// Large per-thread array with dynamic indexing so it cannot live in registers;
// forces a large private (scratch) segment.
__global__ void vscratch(float* out, int n, int iters) {
    float big[256];
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    for (int k = 0; k < 256; ++k)
        big[k] = (float)(i + k);
    float acc = 0.f;
    for (int it = 0; it < iters; ++it) {
        int idx = (i + it) & 255;
        big[idx] += acc * 0.5f + 1.0f;
        acc += big[(idx * 7 + 3) & 255];
    }
    if (i < n)
        out[i] = acc;
}

int main(int argc, char** argv) {
    int n = 1 << 20, loops = 4;
    if (argc > 1)
        loops = atoi(argv[1]);
    float* out;
    if (hipMalloc(&out, (size_t)n * sizeof(float)) != hipSuccess)
        return 1;
    dim3 block(256), grid((n + 255) / 256);
    for (int l = 0; l < loops; ++l) {
        for (int b = 0; b < 3; ++b)
            hipLaunchKernelGGL(vscratch, grid, block, 0, 0, out, n, 64);
        hipDeviceSynchronize();
        fprintf(stderr, "scratch-test: loop=%d\n", l);
        usleep(150000);
    }
    hipFree(out);
    fprintf(stderr, "scratch-test: done (grid_items=%d)\n", grid.x * block.x);
    return 0;
}
