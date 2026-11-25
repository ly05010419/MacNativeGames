/* Exact shader layouts only: TOTK 1.4.3 and Breath of the Wild 1.9.0. Original SDR instructions remain in
 * the disabled branch. Algorithm references and license: LICENSE.renodx.
 * Scene grading controls follow the RenoDX neutral path; this is a Metal port.
 */
#pragma once
#import <Foundation/Foundation.h>
#define TOTK_HDR_COMPOSE_KEY "0ac9a54938974396134d911dc0f29c79ce43e3d8cd50b11ee8b85a48ead6d2a5-"
#define TOTK_HDR_POST_KEY "ac58fc18eda5f3878b16074dc28cd441644a66710f1b73954d39e57ac357561b-"
#define BOTW_HDR_COMPOSE_KEY "f7ee6fe10b925543e905dfc9437ce67dc21a118d465b03bb3c9eccafdf2e33c4-"   /* 1.9.0 (TAA_COMPOSE in nvn_obj.h) */
static NSString *totk_hdr_scene_msl(void) {
    return @"\n"
        "// The HDR functions take relaxed math (fast, keeping NaN and infinity): with the folded\n"
        "// matrices below, the BotW compose at 1080p went from 1.12 to 0.29 ms of GPU time. The\n"
        "// game's code after them goes back to the runtime's safe mode.\n"
        "#pragma METAL fp math_mode(relaxed)\n"
        "// Scene tone mapping: C1 extension of the original exponential toe, SDR hue /\n"
        "// purity donor in MacLeod-Boynton cone coordinates, authored saturation, then\n"
        "// RenoDX Neutwo in BT.2020. All operations precede any SDR clipping/quantization.\n"
        "float hdr_curve(float x) {\n"
        "    return x < 0.001 ? x * (1.0 - 0.5*x + x*x/6.0) : 1.0-exp(-x);\n"
        "}\n"
        "float hdr_extended(float x,float pivot) {\n"
        "    return x<=pivot ? hdr_curve(x) : hdr_curve(pivot)+exp(-pivot)*(x-pivot);\n"
        "}\n"
        "float3 hdr_2020(float3 c) {\n"
        "    return float3(dot(c,float3(0.627403896,0.329283038,0.043313066)),\n"
        "                  dot(c,float3(0.069097289,0.919540395,0.011362316)),\n"
        "                  dot(c,float3(0.016391439,0.088013308,0.895595253)));\n"
        "}\n"
        "float3 hdr_709(float3 c) {\n"
        "    return float3(dot(c,float3(1.660491002,-0.587641139,-0.072849863)),\n"
        "                  dot(c,float3(-0.124550475,1.132899897,-0.008349423)),\n"
        "                  dot(c,float3(-0.018150763,-0.100578898,1.118729661)));\n"
        "}\n"
        "// MacLeod-Boynton purity donor. The chain BT.709 -> BT.2020 -> XYZ -> Stockman-Sharpe\n"
        "// LMS (CIE 2006, as in RenoDX's MB model) and back is folded into one matrix each way,\n"
        "// and the gamut boundary's constants are precomputed (double precision).\n"
        "constant float3x3 hdr_709_lms=float3x3(float3(0.2896057763,0.09018375735,0.0155287402),float3(0.6972466439,0.7073490255,0.05360930734),float3(0.07637127901,0.1122031468,0.5097978304));\n"
        "constant float3x3 hdr_lms_709=float3x3(float3(4.96761965,-0.6196828801,-0.0861520031),float3(-4.922379529,2.051750652,-0.06581938376),float3(0.3391991818,-0.3587439852,1.988954483));\n"
        "// Distance to the BT.2020 nonnegative gamut boundary at fixed L+M, from the white point\n"
        "// along a unit MB direction. Purity relative to that boundary avoids RGB/HSV hue\n"
        "// interpolation artifacts. The BT.2020 white and the two axes are constants.\n"
        "float hdr_mb_radius(float2 direction) {\n"
        "    const float3 base=float3(0.5068527432,0.5068527431,0.506852743);\n"
        "    float3 delta=direction.x*float3(5.324485598,-1.773349934,-0.09122018381)+direction.y*float3(0.1808342948,-0.2838427127,1.755283912);\n"
        "    float3 bound=select(float3(1e10),base/max(-delta,float3(1e-12)),delta<-1e-12);\n"
        "    return max(min(bound.x,min(bound.y,bound.z)),1e-8);\n"
        "}\n"
        "float3 hdr_mb_donor(float3 target709,float3 donor709,float hueStrength,float purityStrength) {\n"
        "    float3 a=hdr_709_lms*target709;\n"
        "    float3 b=hdr_709_lms*donor709;\n"
        "    float at=a.x+a.y,bt=b.x+b.y;\n"
        "    if(at<=1e-7 || bt<=1e-7)return target709;\n"
        "    const float2 white=float2(0.5388978485,0.2934352378);\n"
        "    float2 ad=a.xz/at-white,bd=b.xz/bt-white;\n"
        "    float al=length(ad),bl=length(bd);\n"
        "    if(al<1e-8)return target709;\n"
        "    float3 donor=max(donor709,float3(0));\n"
        "    float chroma=(max(donor.r,max(donor.g,donor.b))-min(donor.r,min(donor.g,donor.b)))/max(max(donor.r,max(donor.g,donor.b)),1e-7);\n"
        "    float hueWeight=bl>1e-8?hueStrength*smoothstep(0.02,0.12,chroma):0.0;\n"
        "    float2 direction=mix(ad/al,bd/max(bl,1e-8),hueWeight);\n"
        "    float dl=length(direction);if(dl<1e-8)return target709;\n"
        "    direction/=dl;\n"
        "    float ap=al/hdr_mb_radius(ad/al),bp=bl>1e-8?bl/hdr_mb_radius(bd/bl):0.0;\n"
        "    float purity=clamp(mix(ap,bp,purityStrength),0.0,1.0);\n"
        "    float2 mb=white+direction*hdr_mb_radius(direction)*purity;\n"
        "    return hdr_lms_709*(float3(mb.x,1.0-mb.x,mb.y)*at);\n"
        "}\n"
        "float3 hdr_scene_map(float3 raw,float3 weights,float gradeWeight,float2 grade,float peak,float4 tone,float4 chroma,float4 art) {\n"
        "    raw=select(max(raw,float3(0)),float3(0),!isfinite(raw));\n"
        "    float y=dot(raw,weights),sdrY=hdr_curve(y);\n"
        "    float blend=sdrY*sdrY;\n"
        "    float3 donor=mix(raw*(y>1e-7?sdrY/y:1.0),float3(hdr_curve(raw.r),hdr_curve(raw.g),hdr_curve(raw.b)),blend);\n"
        "    float ey=hdr_extended(y,chroma.z);\n"
        "    float3 color=mix(raw*(y>1e-7?ey/y:1.0),float3(hdr_extended(raw.r,chroma.z),hdr_extended(raw.g,chroma.z),hdr_extended(raw.b,chroma.z)),blend);\n"
        "    color=hdr_mb_donor(color,clamp(donor,0.0,1.0),chroma.w,art.x);\n"
        "    // Evaluate the game's saturation on a bounded max-channel proxy, then\n"
        "    // restore the unclipped lighting range. This is RenoDX's grade separation.\n"
        "    float m=max(abs(color.r),max(abs(color.g),abs(color.b)));\n"
        "    float scale=rsqrt(1.0+m*m);\n"
        "    float3 proxy=color*scale;\n"
        "    float triangle=dot(proxy,float3(0.6666666865,gradeWeight,gradeWeight))-1.0;\n"
        "    float saturation=(1.0-triangle*triangle)*grade.y*art.y+grade.x;\n"
        "    float maxch=max(proxy.r,max(proxy.g,proxy.b));\n"
        "    color=(maxch+(proxy-maxch)*saturation)/scale;\n"
        "    float3 wide=max(hdr_2020(color),float3(0));\n"
        "    // RenoDX-style luminance grading, with neutral controls as the identity.\n"
        "    float lum=dot(wide,float3(0.262700212,0.677998072,0.059301717));\n"
        "    if(lum>0.0) {\n"
        "        float target=lum*tone.x,mid=0.18;\n"
        "        if(tone.y>1.0)target=max(target,mix(target,mid*pow(target/mid,tone.y),target));\n"
        "        else if(tone.y<1.0)target=min(target,target/(1.0+mid*pow(target/mid,2.0-tone.y)-target));\n"
        "        // Anchor the shadow adjustment at middle gray and keep black at zero.\n"
        "        if(tone.z!=1.0 && target<mid)target=mix(mid*pow(max(target/mid,0.0),1.0/tone.z),target,smoothstep(0.0,mid,target));\n"
        "        if(tone.w!=1.0)target=mid*pow(max(target/mid,0.0),tone.w);\n"
        "        wide*=target/lum;\n"
        "        wide=max(mix(float3(target),wide,chroma.x),float3(0));\n"
        "    }\n"
        "    // RenoDX Neutwo: p*x/sqrt(x*x+p*p), monotone with a finite display peak.\n"
        "    // Copyright (C) 2026 Carlos Lopez, MIT; see LICENSE.renodx.\n"
        "    float p=max(peak,0.25);\n"
        "    // Reciprocal form avoids x*x overflow at extreme user grading settings.\n"
        "    float3 q=wide/p,r=p/max(wide,float3(1e-20));\n"
        "    wide=p*select(q*rsqrt(float3(1)+q*q),rsqrt(float3(1)+r*r),wide>p);\n"
        "    return hdr_709(wide);\n"
        "}\n"
        "#pragma METAL fp math_mode(safe)\n";   /* safe: the runtime's mode (nvn_mtl.m); under TOTK_FAST_MATH=1 the game's
                                                  code in a twin still compiles safe, unlike its other shaders */
}
static NSString *totk_hdr_uniform(NSString *s) {
    if(![s containsString:@"[[buffer(0)]]"] || [s containsString:@"[[buffer(30)]]"])return nil;
    s=[s stringByReplacingOccurrencesOfString:@"using namespace metal;" withString:@"using namespace metal;\nstruct TotkHDRUniform { float4 display; float4 tone; float4 color; float4 art; };\n"];
    return [s stringByReplacingOccurrencesOfString:@"[[buffer(0)]]" withString:@"[[buffer(0)]], constant TotkHDRUniform& hdrConfig [[buffer(30)]]"];
}
static inline NSString *totk_hdr_compose_source(NSString *source) {
    NSString *anchor=@"    out.m_34.w = 0.0;";
    if(![source containsString:anchor] ||
       ![source containsString:@"float _69 = _63 + _57;"] ||
       ![source containsString:@"float _71 = _65 + _59;"] ||
       ![source containsString:@"float _73 = _67 + _61;"] ||
       ![source containsString:@"(*spvDescriptorSet0.fp_c3)._m0[0].y"])return nil;
    NSString *s=totk_hdr_uniform(source);if(!s)return nil;
    s=[s stringByReplacingOccurrencesOfString:@"using namespace metal;" withString:[@"using namespace metal;\n" stringByAppendingString:totk_hdr_scene_msl()]];
    NSString *body=@"    if (hdrConfig.display.x > 0.5) {\n"
      "        out.m_34.xyz = hdr_scene_map(float3(_63*hdrConfig.color.y+_57,_65*hdrConfig.color.y+_59,_67*hdrConfig.color.y+_61), float3(0.2989000082,(*spvDescriptorSet0.fp_c1)._m0[0].x,(*spvDescriptorSet0.fp_c1)._m0[0].y), (*spvDescriptorSet0.fp_c1)._m0[0].z, (*spvDescriptorSet0.fp_c3)._m0[0].xy, hdrConfig.display.y, hdrConfig.tone, hdrConfig.color, hdrConfig.art) * hdrConfig.display.z;\n    }\n";
    return [s stringByReplacingOccurrencesOfString:anchor withString:[body stringByAppendingString:anchor]];
}
/* Breath of the Wild 1.9.0's compose f7ee6fe1 (uking_pass_shader.sharcb, AGL hdr_compose) is the same tone map as
 * TOTK's: min(scene, 10000) + bloom, the 1-exp(-x) luminance / per-channel mix, the grade from c3[0].xy. Only the
 * translator's numbering differs: the scene is _63/_65/_71 (slot 0), the bloom _57/_59/_61 (slot 1), their sums
 * _67/_69/_75. The bloom setting scales the bloom texture. */
static inline NSString *botw_hdr_compose_source(NSString *source) {
    NSString *anchor=@"    out.m_34.w = 0.0;";
    if(![source containsString:anchor] ||
       ![source containsString:@"float _63 = precise::min(_49, 10000.0);"] ||
       ![source containsString:@"float _65 = precise::min(_51, 10000.0);"] ||
       ![source containsString:@"float _71 = precise::min(_53, 10000.0);"] ||
       ![source containsString:@"float _67 = _63 + _57;"] ||
       ![source containsString:@"float _69 = _65 + _59;"] ||
       ![source containsString:@"float _75 = _71 + _61;"] ||
       ![source containsString:@"(*spvDescriptorSet0.fp_c3)._m0[0].y"])return nil;
    NSString *s=totk_hdr_uniform(source);if(!s)return nil;
    s=[s stringByReplacingOccurrencesOfString:@"using namespace metal;" withString:[@"using namespace metal;\n" stringByAppendingString:totk_hdr_scene_msl()]];
    NSString *body=@"    if (hdrConfig.display.x > 0.5) {\n"
      "        out.m_34.xyz = hdr_scene_map(float3(_57*hdrConfig.color.y+_63,_59*hdrConfig.color.y+_65,_61*hdrConfig.color.y+_71), float3(0.2989000082,(*spvDescriptorSet0.fp_c1)._m0[0].x,(*spvDescriptorSet0.fp_c1)._m0[0].y), (*spvDescriptorSet0.fp_c1)._m0[0].z, (*spvDescriptorSet0.fp_c3)._m0[0].xy, hdrConfig.display.y, hdrConfig.tone, hdrConfig.color, hdrConfig.art) * hdrConfig.display.z;\n    }\n";
    return [s stringByReplacingOccurrencesOfString:anchor withString:[body stringByAppendingString:anchor]];
}
/* EASU's 1/3.5 input and 3.5 output scaling are retained; its final SDR bounds
 * are removed, including negative extended-sRGB channels. Edge weights retain
 * their original clamps. */
static inline NSString *totk_hdr_post_source(NSString *source) {
    NSString *s=totk_hdr_uniform(source);if(!s)return nil;
    // The v9 translator emits precise::clamp where v8 emitted fast::clamp.
    NSString *ns = [s containsString:@"precise::clamp(_785, 0.0, 1.0)"] ? @"precise" : @"fast";
    for(NSString *v in @[@"_785",@"_789",@"_793"]) {
        NSString *old=[NSString stringWithFormat:@"%@::clamp(%@, 0.0, 1.0)",ns,v];
        if(![s containsString:old])return nil;
        s=[s stringByReplacingOccurrencesOfString:old withString:[NSString stringWithFormat:@"(hdrConfig.display.x > 0.5 ? %@ : %@)",v,old]];
    }
    return s;
}
/* Which shaders take an HDR twin in the game that is built: 1 the scene compose, 2 the postprocess whose SDR clamps are
 * lifted, 0 none. BotW 1.9.0 has no postprocess twin: its FXAA bd7ffdd4 has no clamp. Other BotW versions have none. */
static inline int hdr_twin_kind(NSString *key) {
#if defined(GAME_BOTW190)
    return [key hasPrefix:@BOTW_HDR_COMPOSE_KEY] ? 1 : 0;
#elif defined(GAME_BOTW)
    (void)key; return 0;
#else
    return [key hasPrefix:@TOTK_HDR_COMPOSE_KEY] ? 1 : [key hasPrefix:@TOTK_HDR_POST_KEY] ? 2 : 0;
#endif
}
static inline NSString *hdr_twin_source(int kind, NSString *source) {
#if defined(GAME_BOTW)
    return kind == 1 ? botw_hdr_compose_source(source) : nil;
#else
    return kind == 1 ? totk_hdr_compose_source(source) : totk_hdr_post_source(source);
#endif
}
