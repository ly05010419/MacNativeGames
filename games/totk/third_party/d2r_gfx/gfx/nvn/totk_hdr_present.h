#pragma once
/* Native FP16 scene is linear BT.709/extended-sRGB, including negative channels
 * for colors outside BT.709. Decode only legacy SDR/overlay input, exactly once. */
static NSString *totk_hdr_copy_source(void) {
    return @"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct V{float4 p [[position]];};"
      "vertex V vs(uint i [[vertex_id]]){float2 u=float2((i<<1)&2,i&2);return{float4(u*2-1,0,1)};}"
      "fragment float4 fs(V v [[stage_in]],texture2d<float> t [[texture(0)]]){return t.read(uint2(v.p.xy));}";
}
/* A picture larger than its destination (a render above the window or screen size) is supersampling: each screen pixel
 * averages every texel it covers, in linear light (present_fetch). One bilinear tap per pixel uses them all only at
 * exactly 2x, and averages the stored gamma values. At or below the destination size the single tap is kept, so 1:1,
 * upscales and the overlay boxes drawn with this pipeline present as before. TOTK_PRESENT_BOX=0 keeps the single tap
 * everywhere. Read when the pipeline is built. */
static int present_box_on(void) { const char *e = getenv("TOTK_PRESENT_BOX"); return !(e && !strcmp(e, "0")); }
static NSString *totk_hdr_present_source(MTLPixelFormat format, BOOL hdrAvailable) {
    static const char *original=
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
        "struct Crop { float4 rect; float4 edge; };\n"
        "float3 present_srgb_decode(float3 c) { return select(c/12.92,pow((max(c,float3(0))+0.055)/1.055,float3(2.4)),c>0.04045); }\n"
        "float3 present_srgb_encode(float3 c) { return select(c*12.92,1.055*pow(max(c,float3(0)),float3(1.0/2.4))-0.055,c>0.0031308); }\n"
        // mode 0: average the stored values (linear FP16); 1: decode sRGB per texel, average, encode; 2: decode, return linear
        "float4 present_fetch(texture2d<float> t, sampler s, float2 uv, float4 edge, uint mode) {\n"
        "    float2 size = float2(t.get_width(), t.get_height());\n"
        "    float2 f = float2(abs(dfdx(uv.x)), abs(dfdy(uv.y))) * size;   // texels per screen pixel\n"
        "    if (!PRESENT_BOX || (f.x <= 1.01 && f.y <= 1.01)) {\n"
        "        float4 v = t.sample(s, clamp(uv, edge.xy, edge.zw));\n"
        "        return mode == 2 ? float4(present_srgb_decode(v.rgb), v.a) : v;\n"
        "    }\n"
        "    f = clamp(f, 1.0, 4.0);\n"
        "    float2 p = uv * size, lo = max(p - f * 0.5, edge.xy * size - 0.5), hi = min(p + f * 0.5, edge.zw * size + 0.5);\n"
        "    int2 i0 = int2(floor(lo)), i1 = int2(ceil(hi)) - 1;\n"
        "    float4 sum = 0; float wsum = 0;\n"
        "    for (int y = i0.y; y <= i1.y && y < i0.y + 6; y++) {\n"
        "        float wy = min(hi.y, float(y + 1)) - max(lo.y, float(y));\n"
        "        for (int x = i0.x; x <= i1.x && x < i0.x + 6; x++) {\n"
        "            float w = (min(hi.x, float(x + 1)) - max(lo.x, float(x))) * wy;\n"
        "            float4 v = t.read(uint2(x, y));\n"
        "            if (mode != 0) v.rgb = present_srgb_decode(v.rgb);\n"
        "            sum += v * w; wsum += w;\n"
        "        }\n"
        "    }\n"
        "    float4 a = sum / max(wsum, 1e-6);\n"
        "    if (mode == 1) a.rgb = present_srgb_encode(a.rgb);\n"
        "    return a;\n"
        "}\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return present_fetch(t, s, in.uv * c.rect.zw + c.rect.xy, c.edge, 1); }\n";
    NSString *source=[NSString stringWithFormat:@"#define PRESENT_BOX %d\n%s",present_box_on(),original];
    if(!hdrAvailable)return source;
    BOOL edr=format==MTLPixelFormatRGBA16Float;
    NSString *helpers=@"\n"
        "float3 hdr_decode(float3 c) {\n"
        "    return select(c/12.92,pow((max(c,float3(0))+0.055)/1.055,float3(2.4)),c>0.04045);\n"
        "}\n"
        "float3 hdr_encode(float3 c) {\n"
        "    return select(c*12.92,1.055*pow(max(c,float3(0)),float3(1.0/2.4))-0.055,c>0.0031308);\n"
        "}\n"
        "// Bound the display gamut on a constant-luminance ray. The scene has already\n"
        "// been tone mapped: applying another highlight shoulder here would remap it.\n"
        "// Convert primaries numerically; the layer keeps extended-linear-sRGB metadata.\n"
        "float3 hdr_display_bound(float3 c,float headroom) {\n"
        "    c=select(c,float3(0),!isfinite(c));\n"
        "    float3 p=float3(dot(c,float3(0.822461969,0.177538031,0)),\n"
        "                    dot(c,float3(0.033194199,0.966805801,0)),\n"
        "                    dot(c,float3(0.017082631,0.072397441,0.910519928)));\n"
        "    float h=max(headroom,1.0);\n"
        "    float y=clamp(dot(p,float3(0.228974564,0.691738522,0.079286914)),0.0,h);\n"
        "    float3 d=p-y;\n"
        "    float scale=1.0;\n"
        "    for(uint i=0;i<3;i++) {\n"
        "        if(d[i]>1e-7)scale=min(scale,(h-y)/d[i]);\n"
        "        if(d[i]<-1e-7)scale=min(scale,-y/d[i]);\n"
        "    }\n"
        "    p=y+d*max(scale,0.0);\n"
        "    return float3(dot(p,float3(1.224940176,-0.224940176,0)),\n"
        "                  dot(p,float3(-0.042056955,1.042056955,0)),\n"
        "                  dot(p,float3(-0.019637555,-0.078636046,1.098273601)));\n"
        "}\n";
    source=[source stringByReplacingOccurrencesOfString:@"struct Crop" withString:[helpers stringByAppendingString:@"struct Crop"]];
    if(edr)source=[source stringByReplacingOccurrencesOfString:@"present_fetch(t, s, in.uv * c.rect.zw + c.rect.xy, c.edge, 1)" withString:@"present_fetch(t, s, in.uv * c.rect.zw + c.rect.xy, c.edge, 2)"];
    source=[source stringByAppendingFormat:@"\nstruct HDRCrop{float4 rect;float4 edge;float headroom;float uiWhite;};\nfragment float4 fshdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant HDRCrop& c [[buffer(0)]]) {float3 v=present_fetch(t,s,in.uv*c.rect.zw+c.rect.xy,c.edge,0).rgb; float3 rgb=hdr_display_bound(v*c.uiWhite,c.headroom);return float4(%@,1);}",edr?@"rgb":@"hdr_encode(rgb)"];
    return source;
}
