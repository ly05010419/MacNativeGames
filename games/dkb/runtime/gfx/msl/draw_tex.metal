// nvnCommandBufferDrawTexture drawn as a render pass (nvn_mtl.m draw_tex)
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
// p[0] = destination origin + source texels per destination pixel, p[1] = source origin + source level size
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[1].xy + (in.p.xy - p[0].xy) * p[0].zw; return t.sample(s, st / p[1].zw); }
