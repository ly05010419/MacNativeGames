#import "vr_labo_shader.h"
#include <TargetConditionals.h>
#include <stdio.h>

id<MTLFunction> vr_labo_quad_vertex(id<MTLDevice> device, int tone) {
#if defined(GAME_BOTW) && TARGET_OS_OSX
    static id<MTLFunction> function, tone_function;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        /* The native vertex shader's sole argument is cbuf0 at argument id0.
         * Its output locn0.xy is the sample UV; .zw is the FXAA neighbour UV.
         * These expressions are verified against the original full shader hash
         * in vr_labo_shader_pair; shader member names need not be stable. */
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct A { constant float4 *c [[id(0)]]; };\n"
            "struct O { float4 uv [[user(locn0)]]; float4 p [[position]]; };\n"
            "vertex O labo_quad(uint i [[vertex_id]], constant A &a [[buffer(0)]]) {\n"
            " float2 uv = float2(i >> 1u, i & 1u); O o;\n"
            " o.p = float4(uv.x * 2 - 1, 1 - uv.y * 2, a.c[5].y * 0.5 + 0.5, 1);\n"
            " o.uv = float4(uv, uv - (a.c[4].x + 0.5) * a.c[0].xy); return o; }\n"
            "vertex O tone_quad(uint i [[vertex_id]]) { float2 uv = float2(i >> 1u, i & 1u); O o;\n"
            " o.p = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0.5, 1); o.uv = float4(uv, 0, 1); return o; }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        function = [library newFunctionWithName:@"labo_quad"];
        tone_function = [library newFunctionWithName:@"tone_quad"];
        if (!function) fprintf(stderr, "[vr-labo] lens-free vertex function failed: %s\n", error.localizedDescription.UTF8String);
    });
    return tone ? tone_function : function;
#else
    (void)device; (void)tone; return nil;
#endif
}

id<MTLFunction> vr_native_output_fragment(id<MTLDevice> device) {
#if defined(GAME_BOTW) && TARGET_OS_OSX
    static id<MTLFunction> function;static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct A { constant float4 *c0 [[id(0)]]; constant float4 *c1 [[id(1)]]; texture2d<float> lut [[id(2)]]; sampler lut_s [[id(3)]]; texture2d<float> image [[id(4)]]; sampler image_s [[id(5)]]; };\n"
            "struct O { float4 uv [[user(locn0)]]; };\n"
            "fragment float4 native_output(O in [[stage_in]],constant A &a [[buffer(0)]]) {return float4(a.image.sample(a.image_s,in.uv.xy).rgb,1); }\n";
        NSError *error=nil;id<MTLLibrary> library=[device newLibraryWithSource:source options:nil error:&error];
        function=[library newFunctionWithName:@"native_output"];
        if(!function)fprintf(stderr,"[vr] plain native output function failed: %s\n",error.localizedDescription.UTF8String);
    });return function;
#else
    (void)device;return nil;
#endif
}
