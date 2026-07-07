#include <hip/hip_runtime.h>
#include <cstdio>
__global__ void vadd(float*a,float*b,float*c,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n)c[i]=a[i]+b[i];}
__global__ void vscale(float*a,float s,int n){int i=blockIdx.x*blockDim.x+threadIdx.x;if(i<n)a[i]*=s;}
int main(){int n=1<<24;size_t s=n*sizeof(float);float*a,*b,*c;hipMalloc(&a,s);hipMalloc(&b,s);hipMalloc(&c,s);
for(int k=0;k<100;k++){ // no per-iter sync: let the queue fill up
  hipLaunchKernelGGL(vadd,dim3((n+255)/256),dim3(256),0,0,a,b,c,n);
  hipLaunchKernelGGL(vscale,dim3((n+255)/256),dim3(256),0,0,c,2.0f,n);
}
hipDeviceSynchronize();printf("deep done\n");return 0;}
