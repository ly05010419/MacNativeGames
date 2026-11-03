// The 4 Maxwell compute shaders embedded in a game's main program, written in MSL (nvn_builtin.h, nvn_mtl.m g_builtin_lib)
#include <metal_stdlib>
using namespace metal;
#define HEAD uint id = (cta.x << 5) + tid.x; if (id >= n[0]) return;
#define LD(o) (*(device const ulong *)(src + (o)))
#define ARGS constant uint *n [[buffer(0)]], device const uchar *src [[buffer(1)]], device uchar *dst [[buffer(2)]], uint3 cta [[threadgroup_position_in_grid]], uint3 tid [[thread_position_in_threadgroup]]
#define ST(v) (*(device ulong *)(dst + (id << 3)) = (v))
kernel void q_val0(ARGS) { HEAD ST(LD(id << 4)); }
kernel void q_val8(ARGS) { HEAD ST(LD((id << 4) + 8u)); }
kernel void q_dlt0(ARGS) { HEAD uint b = id << 5; ST(LD(b + 16u) - LD(b)); }
kernel void q_dlt8(ARGS) { HEAD uint b = (id << 5) + 16u; ST(LD(b + 8u) - LD(b - 8u)); }
