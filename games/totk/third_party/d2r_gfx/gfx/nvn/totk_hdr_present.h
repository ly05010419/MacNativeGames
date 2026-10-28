#pragma once
/* Native FP16 scene is linear BT.709/extended-sRGB, including negative channels
 * for colors outside BT.709. Decode only legacy SDR/overlay input, exactly once. */
static NSString *totk_hdr_copy_source(void) {
    return @"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct V{float4 p [[position]];};"
      "vertex V vs(uint i [[vertex_id]]){float2 u=float2((i<<1)&2,i&2);return{float4(u*2-1,0,1)};}"
      "fragment float4 fs(V v [[stage_in]],texture2d<float> t [[texture(0)]]){return t.read(uint2(v.p.xy));}";
}
static NSString *totk_hdr_present_source(MTLPixelFormat format, BOOL hdrAvailable) {
    static const char *original=
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
        "struct Crop { float4 rect; float4 edge; };\n"
        "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
    NSString *source=@(original);
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
    if(edr)source=[source stringByReplacingOccurrencesOfString:@"return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));" withString:@"float4 v=t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));return float4(hdr_decode(v.rgb),v.a);"];
    source=[source stringByAppendingFormat:@"\nstruct HDRCrop{float4 rect;float4 edge;float headroom;float uiWhite;};\nfragment float4 fshdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant HDRCrop& c [[buffer(0)]]) {float3 v=t.sample(s,clamp(in.uv*c.rect.zw+c.rect.xy,c.edge.xy,c.edge.zw)).rgb; float3 rgb=hdr_display_bound(v*c.uiWhite,c.headroom);return float4(%@,1);}",edr?@"rgb":@"hdr_encode(rgb)"];
    return source;
}
