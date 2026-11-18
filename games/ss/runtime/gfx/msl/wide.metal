// 8-bit indices widened to 16-bit on the GPU (nvn_mtl.m wide_run)
#include <metal_stdlib>
using namespace metal;
struct wjob { device const uchar *src; device ushort *dst; uint n, shift; };
kernel void wide_u8(device const wjob &j [[buffer(0)]], uint i [[thread_position_in_grid]]) { if (i < j.n) j.dst[i] = ushort(j.src[j.shift + i]); }
