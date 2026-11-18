#pragma once
/* NVIDIA's float blender with "zero times anything is zero" on (Ocarina of Time, 2026-11-13).

   NVN's driver sets the 3D engine's SET_BLEND_FLOAT_OPTION (ZERO_TIMES_ANYTHING_IS_ZERO) in its start-up command list, so on
   the console a blend term whose weight is 0 adds 0, whatever the colour. Apple's blender multiplies as IEEE does: a NaN or
   Inf colour under a zero weight reaches the target. Ocarina of Time's mist shader 5ad65c0eefd09c7f divides by a zero
   density sum and writes the result with alpha 0 into an RGBA16F target blended SRC_ALPHA / ONE_MINUS_SRC_ALPHA; the
   composite carries the NaN into the scene and the bloom spreads it into white clouds over the ground (checked here: the
   whole-screen white in Kokiri Forest goes away with that one fragment shader skipped, flag skipfs_5ad65c0eefd09c7f).
   Diagnosis and design from seedds/nxnative (MIT) 0662f9d8, PR #2030, issue #1839; written anew for this layer.

   Metal has no such blend option, so a translated pixel shader zeroes a non-finite source colour itself where its weight
   is zero (msl_blend_zero, at translation: totk_shader_rt.m), and the host tells it per draw which outputs have such a
   weight (blend_zero_bits, bound by nvn_mtl.m bz_bind at fragment buffer BLEND_ZERO_BUFFER): BLEND_ZERO_BITS bits per
   colour output n ([[color(n)]], the Metal attachment), 1 = zero where its alpha is 0 (SRC_ALPHA), 2 = where its alpha is 1
   (ONE_MINUS_SRC_ALPHA), 0 = not known to be zero (the colour is returned as it is). A finite colour and the alpha are never
   changed. A factor of ZERO needs nothing (Apple leaves that term out); a destination factor that reads the source colour
   keeps 0, since zeroing the source would change that term too. Not covered (a shader cannot see it): a non-finite value
   already in the target under a zero destination weight, and a non-finite weight on a colour of 0. */
#include <stdint.h>

#define BLEND_ZERO_BUFFER 28   // fragment buffer: translated shaders use buffer 0 only; 29 is TOTK_NAN_TRACE, 30 the HDR twin's
#define BLEND_ZERO_BITS 2

/* The bits of one colour output from its blend state (nvn.h values: factors SRC_COLOR 3, ONE_MINUS_SRC_COLOR 4, SRC_ALPHA 5,
   ONE_MINUS_SRC_ALPHA 6, SRC1_* 0x10-0x13; equations MIN 4, MAX 5). float_target: a format that stores NaN. */
static inline uint32_t blend_zero_bits(int blending, int float_target, unsigned src_rgb, unsigned dst_rgb, unsigned eq_rgb) {
    if (!blending || !float_target || eq_rgb == 4 || eq_rgb == 5) return 0;
    if (dst_rgb == 3 || dst_rgb == 4 || (dst_rgb >= 0x10 && dst_rgb <= 0x13)) return 0;
    return src_rgb == 5 ? 1 : src_rgb == 6 ? 2 : 0;
}

#ifdef __OBJC__
#import <Foundation/Foundation.h>

/* The helper the guard calls: a float4 / half4 colour whose weight is zero (its mode bits and alpha say so) gets 0 in
   place of each non-finite rgb component, found by its exponent bits (fast math may assume no NaN exists); any other
   type (an output msl_retype_outputs made integer) is returned as it is. */
#define BLEND_ZERO_HELPER \
    "template<typename T> static inline __attribute__((always_inline)) T dm_bz(T c, uint m) { return c; }\n" \
    "static inline __attribute__((always_inline)) float4 dm_bz(float4 c, uint m)\n{\n" \
    "    m &= 3u;\n    if (!((m == 1u && c.w == 0.0f) || (m == 2u && c.w == 1.0f))) return c;\n" \
    "    return float4(select(c.xyz, float3(0.0f), (as_type<uint3>(c.xyz) & uint3(0x7f800000u)) == uint3(0x7f800000u)), c.w);\n}\n" \
    "static inline __attribute__((always_inline)) half4 dm_bz(half4 c, uint m)\n{\n" \
    "    m &= 3u;\n    if (!((m == 1u && c.w == 0.0h) || (m == 2u && c.w == 1.0h))) return c;\n" \
    "    return half4(select(c.xyz, half3(0.0h), (as_type<ushort3>(c.xyz) & ushort3(0x7c00u)) == ushort3(0x7c00u)), c.w);\n}\n"

/* The MSL of a translated pixel shader with the guard: before each `return out;` line of main0 each float4 / half4 colour
   output is passed through dm_bz with its bits of the word at [[buffer(BLEND_ZERO_BUFFER)]], a new last parameter of
   main0. Written inline on out.<name>, so msl_retype_outputs (which renames the outputs and rewrites out.<name>) still
   applies. *guarded: the returns guarded; the MSL comes back unchanged (and 0) for a shader without such an output, with
   a return that is not `return out;` on a line of its own, or of another shape than a SPIRV-Cross fragment main0. */
static NSString *msl_blend_zero(NSString *msl, int *guarded) {
    *guarded = 0;
    if (!msl || [msl containsString:@"dm_bz("]) return msl;
    NSRange st = [msl rangeOfString:@"struct main0_out\n{\n"];
    if (st.location == NSNotFound) return msl;
    NSRange stEnd = [msl rangeOfString:@"\n};" options:0 range:NSMakeRange(NSMaxRange(st), msl.length - NSMaxRange(st))];
    if (stEnd.location == NSNotFound) return msl;
    NSString *members = [msl substringWithRange:NSMakeRange(NSMaxRange(st), stEnd.location - NSMaxRange(st))];
    NSRegularExpression *out = [NSRegularExpression regularExpressionWithPattern:@"^\\s*(float4|half4)\\s+(\\w+)\\s*\\[\\[color\\((\\d+)\\)\\]\\];"
                                                                        options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSMutableString *calls = [NSMutableString string];
    for (NSTextCheckingResult *m in [out matchesInString:members options:0 range:NSMakeRange(0, members.length)]) {
        NSString *name = [members substringWithRange:[m rangeAtIndex:2]];
        int n = [members substringWithRange:[m rangeAtIndex:3]].intValue;
        if (n < 0 || n >= 8) continue;
        [calls appendFormat:@" out.%@ = dm_bz(out.%@, dm_blend >> %uu);", name, name, (unsigned)(BLEND_ZERO_BITS * n)];
    }
    if (!calls.length) return msl;
    NSRange entry = [msl rangeOfString:@"fragment main0_out main0("];
    if (entry.location == NSNotFound) return msl;
    NSUInteger i = NSMaxRange(entry), depth = 1, len = msl.length;   // the parameter list's closing parenthesis
    for (; i < len && depth; i++) { unichar c = [msl characterAtIndex:i]; if (c == '(') depth++; else if (c == ')') depth--; }
    if (depth) return msl;
    NSUInteger close = i - 1;
    NSRange open = [msl rangeOfString:@"{" options:0 range:NSMakeRange(i, len - i)];
    if (open.location == NSNotFound || [[msl substringWithRange:NSMakeRange(i, open.location - i)] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length) return msl;
    NSUInteger j = open.location + 1; depth = 1;   // the body's closing brace
    for (; j < len && depth; j++) { unichar c = [msl characterAtIndex:j]; if (c == '{') depth++; else if (c == '}') depth--; }
    if (depth) return msl;
    NSRange bodyRange = NSMakeRange(open.location, j - open.location);
    NSString *body = [msl substringWithRange:bodyRange];
    NSUInteger returns = [[NSRegularExpression regularExpressionWithPattern:@"\\breturn\\b" options:0 error:nil] numberOfMatchesInString:body options:0 range:NSMakeRange(0, body.length)];
    NSRegularExpression *ret = [NSRegularExpression regularExpressionWithPattern:@"^([ \\t]*)return out;$" options:NSRegularExpressionAnchorsMatchLines error:nil];
    NSUInteger plain = [ret numberOfMatchesInString:body options:0 range:NSMakeRange(0, body.length)];
    if (!plain || plain != returns) return msl;
    NSString *guard = [NSString stringWithFormat:@"$1if (dm_blend != 0u) {%@ }\n$1return out;", calls];
    body = [ret stringByReplacingMatchesInString:body options:0 range:NSMakeRange(0, body.length) withTemplate:guard];
    NSString *params = [msl substringWithRange:NSMakeRange(NSMaxRange(entry), close - NSMaxRange(entry))];
    NSString *param = [NSString stringWithFormat:@"%@constant uint& dm_blend [[buffer(%d)]]",
                       [params stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet].length ? @", " : @"", BLEND_ZERO_BUFFER];
    NSUInteger lineStart = [msl lineRangeForRange:NSMakeRange(entry.location, 0)].location;   // before "[[ early_fragment_tests ]]" on the entry's line
    NSMutableString *r = [NSMutableString string];
    [r appendString:[msl substringToIndex:lineStart]];
    [r appendString:@BLEND_ZERO_HELPER "\n"];
    [r appendString:[msl substringWithRange:NSMakeRange(lineStart, close - lineStart)]];
    [r appendString:param];
    [r appendString:[msl substringWithRange:NSMakeRange(close, open.location - close)]];
    [r appendString:body];
    [r appendString:[msl substringFromIndex:NSMaxRange(bodyRange)]];
    *guarded = (int)plain;
    return r;
}
#endif
