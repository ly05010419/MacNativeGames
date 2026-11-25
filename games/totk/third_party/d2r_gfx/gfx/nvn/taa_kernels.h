/* Edge smoothing's reactive mask kernels (totk_taa.m compiles them; tests/taa_mask_probe.m runs them on synthetic
   depth). Moved here from totk_taa.m setup() on 2026-11-25, unchanged, so the probe tests the code the game runs. */
#pragma once
static const char taa_kernels_msl[] =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct P { float4x4 reproj; float2 size; float2 inv_size; float prev_valid; };\n"
    // view distance up to the near plane's factor: the depth has an infinite far plane (distance = near / (1 - depth))
    "static float lin(float z) { return 1.0 / max(1.0 - z, 1e-7); }\n"
    // camera motion of each depth pixel: where it was in the previous frame, in pixels of this texture; this frame's
    // depth for the next one; and the reactive mask (see the header)
    "kernel void taa_mv(depth2d<float, access::read> d [[texture(0)]], texture2d<float, access::write> mv [[texture(1)]],\n"
    "                   texture2d<float, access::read> pd [[texture(2)]], texture2d<float, access::write> cd [[texture(3)]],\n"
    "                   texture2d<float, access::write> rm [[texture(4)]],\n"
    "                   constant P &p [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  if (g.x >= uint(p.size.x) || g.y >= uint(p.size.y)) return;\n"
    "  float z = d.read(g);\n"
    "  float2 uv = (float2(g) + 0.5) * p.inv_size;\n"
    "  float4 c = p.reproj * float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, z * 2.0 - 1.0, 1.0);\n"
    "  float2 m = 0.0; float r = 0.0;\n"
    "  if (c.w > 1e-7) {\n"
    "    float3 n = c.xyz / c.w; float2 puv = float2(n.x * 0.5 + 0.5, 0.5 - 0.5 * n.y);\n"
    "    m = (puv - uv) * p.size;\n"
    "    if (p.prev_valid > 0.5 && z < 1.0 && all(puv >= 0.0) && all(puv < 1.0)) {\n"
    "      float le = lin(n.z * 0.5 + 0.5), best = 1e9; int2 q = int2(puv * p.size), hi = int2(p.size) - 1;\n"
    "      for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++)\n"
    "        best = min(best, abs(lin(pd.read(uint2(clamp(q + int2(x, y), int2(0), hi))).x) - le) / le);\n"
    "      r = saturate((best - 0.04) / 0.06);\n"   // 4 % apart starts it, 10 % is the current frame only
    "    }\n"
    "  }\n"
    "  if (any(isnan(m)) || any(isinf(m))) m = 0.0;\n"
    "  if (isnan(r)) r = 0.0;\n"
    "  mv.write(float4(m, 0.0, 0.0), g); cd.write(float4(z, 0.0, 0.0, 0.0), g); rm.write(float4(r, 0.0, 0.0, 0.0), g);\n"
    "}\n";
