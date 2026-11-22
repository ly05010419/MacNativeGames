/* A hard cap on every loop of a translated shader (Age of Calamity, 2026-11-22). A shader whose loop count comes from
   memory the host bound wrongly (a texture buffer read through a 2D slot returned garbage counts) ran for minutes, the
   GPU never finished the command buffer, WindowServer hung in the GPU driver and the Mac panicked; a killed process left
   the work running. With the cap, no loop of a translated shader runs more than MSL_LOOP_CAP times per invocation: the
   picture may be wrong for that draw, the GPU always finishes.

   The rewrite works on SPIRV-Cross's MSL layout: a loop statement ("do", "for (...)", "while (...)") alone on its line,
   its "{" on the next line with the same indentation. Before the loop it declares a counter; the first statement of the
   body counts and leaves the loop past the cap. Nothing else changes; a source without loops comes back as it was.
   Shared (every Zelda-line port's d2r_gfx links it, as hud_panel.inc): a port's nvn_mtl.m calls it where it compiles MSL
   (totk_msl_library). games/totk/ios/tools/msl_loop_cap.py is the same rewrite, byte for byte, for every port's device
   seed (build_port_ipa.py caps the sources before compile_seed.py compiles them); tests/test_msl_loop_cap.py checks both
   on the same sources and which ports call it. */
#pragma once
#import <Foundation/Foundation.h>

#define MSL_LOOP_CAP 65536u

static inline BOOL msl_loop_head(NSString *t) {
    if ([t isEqualToString:@"do"]) return YES;
    if (([t hasPrefix:@"for ("] || [t hasPrefix:@"while ("]) && [t hasSuffix:@")"]) return YES;
    return NO;
}
static inline NSString *msl_loop_cap(NSString *src) {
    if (!src || [src rangeOfString:@"_mng_loop_"].location != NSNotFound) return src;   /* already capped */
    NSArray<NSString *> *lines = [src componentsSeparatedByString:@"\n"];
    NSMutableArray<NSString *> *out = [NSMutableArray arrayWithCapacity:lines.count + 16];
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *line = lines[i];
        NSString *t = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSUInteger indent = [line rangeOfString:t].location;
        NSString *pad = t.length ? [line substringToIndex:indent] : @"";
        BOOL head = t.length && msl_loop_head(t) && i + 1 < lines.count && [lines[i + 1] isEqualToString:[pad stringByAppendingString:@"{"]];
        if (!head) { [out addObject:line]; continue; }
        NSString *v = [NSString stringWithFormat:@"_mng_loop_%lu", (unsigned long)n++];
        [out addObject:[NSString stringWithFormat:@"%@uint %@ = 0u;", pad, v]];
        [out addObject:line];
        [out addObject:lines[++i]];
        [out addObject:[NSString stringWithFormat:@"%@    if (++%@ > %uu) break;", pad, v, MSL_LOOP_CAP]];
    }
    return n ? [out componentsJoinedByString:@"\n"] : src;
}
