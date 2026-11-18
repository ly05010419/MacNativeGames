// Texture copies drawn as a render pass: colour, depth and stencil (nvn_mtl.m copy_t2t_draw)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
// p[0] = source origin (texels) + source texels per destination texel, p[1] = destination origin + source level size
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }
struct D { float d [[depth(any)]]; };
fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.read(uint2(st)); return o; }
struct DS { float d [[depth(any)]]; uint s [[stencil]]; };
fragment DS fsds(V in [[stage_in]], depth2d<float> t [[texture(0)]], texture2d<uint> ts [[texture(1)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; DS o; o.d = t.read(uint2(st)); o.s = ts.read(uint2(st)).r; return o; }
