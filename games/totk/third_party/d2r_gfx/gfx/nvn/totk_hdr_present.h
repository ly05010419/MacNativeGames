#pragma once
/* Production presentation shaders, also used by the HDR GPU probes.
 * Linear output retains sRGB primaries, including values above SDR white. */
static NSString *totk_hdr_present_source(MTLPixelFormat format, BOOL hdrAvailable){
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "struct Crop { float4 rect; float4 edge; };\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant Crop& c [[buffer(0)]]) { return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw)); }\n";
        NSString *source=@(src_msl);
        if(!hdrAvailable)return source;   /* ordinary SDR keeps the upstream shader unchanged */
        BOOL edr=format==MTLPixelFormatRGBA16Float;
        NSString *helpers=@"float3 hdr_decode(float3 c){return select(c/12.92,pow((max(c,float3(0))+0.055)/1.055,float3(2.4)),c>0.04045);}\nfloat3 hdr_encode(float3 c){return select(c*12.92,1.055*pow(max(c,float3(0)),float3(1.0/2.4))-0.055,c>0.0031308);}\nfloat3 hdr_fit(float3 c,float h){c=select(max(c,float3(0)),float3(0),!isfinite(c));h=max(h,1.0);float p=max(c.r,max(c.g,c.b));if(p<=1)return c;if(h<=1)return c/p;float k=max(1.0,0.75*h);float q=p<=k?p:k+(h-k)*(1-exp(-(p-k)/(h-k)));return c*(q/p);}\n";
        source=[source stringByReplacingOccurrencesOfString:@"struct Crop" withString:[helpers stringByAppendingString:@"struct Crop"]];
        if(edr)source=[source stringByReplacingOccurrencesOfString:@"return t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));" withString:@"float4 v=t.sample(s, clamp(in.uv * c.rect.zw + c.rect.xy, c.edge.xy, c.edge.zw));return float4(hdr_decode(v.rgb),v.a);"];
        source=[source stringByAppendingFormat:@"\nstruct HDRCrop{float4 rect;float4 edge;float headroom;};\nfragment float4 fshdr(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant HDRCrop& c [[buffer(0)]]) {float3 v=t.sample(s,clamp(in.uv*c.rect.zw+c.rect.xy,c.edge.xy,c.edge.zw)).rgb; float3 rgb=hdr_fit(v,c.headroom);return float4(%@,1);}",edr?@"rgb":@"hdr_encode(rgb)"];
    return source;
}
