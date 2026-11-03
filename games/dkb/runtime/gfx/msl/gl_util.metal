// The OpenGL layer's own shaders: a quad that clears what a scissored or masked glClear covers (hle_gl.m)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V clr_vs(uint i [[vertex_id]], constant float &z [[buffer(0)]]) { V o; float2 t = float2((i << 1) & 2, i & 2); o.p = float4(t * 2 - 1, z, 1); return o; }
fragment float4 clr_fs(constant float4 &c [[buffer(0)]]) { return c; }
fragment void clr_fs_none() {}
