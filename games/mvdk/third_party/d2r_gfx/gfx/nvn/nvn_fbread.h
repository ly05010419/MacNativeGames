/* TOTK: a decal that blends into the G-buffer samples the albedo / normal attachments of its own pass (ground decals,
   nvn_mtl.m att_fb_take). The Switch renders straight to memory, so each decal sees the decals drawn before it. An Apple GPU
   keeps the pass in tile memory: a sample reads memory, which still holds the pass's start except in tiles the GPU happened
   to store early, so overlapping decals blend differently tile by tile (32 x 32 squares on the floor under the Ultrahand
   close-up in Ukouh Shrine). nvn_fbread_src rewrites a SPIRV-Cross fragment function so that those attachments are read from
   the tile ([[color(n)]], programmable blending) whenever the sampled texel is the fragment's own pixel, and sampled from
   memory as before otherwise. aid / att: argument id of each texture and the colour attachment it is. nil when a texture is
   used other than by plain sample calls, or the source does not have the expected shape. Shared with tests/fbread/probe.m. */
#pragma once
#import <Foundation/Foundation.h>

static NSUInteger nvn_fbread_close(NSString *src, NSRange entry) {   // the parenthesis matching the entry's "(" ([[buffer(0)]] nests)
    int depth = 0;
    for (NSUInteger i = NSMaxRange(entry) - 1; i < src.length; i++) { unichar c = [src characterAtIndex:i];
        if (c == '(') depth++; else if (c == ')' && --depth == 0) return i; }
    return NSNotFound;
}

static NSString *nvn_fbread_src(NSString *src, NSString *name, const int aid[], const int att[], int n) {
    if (!src || !name || n <= 0) return nil;
    NSRegularExpression *entry = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"fragment\\s+\\w+\\s+%@\\s*\\(", name] options:0 error:nil];
    NSRange e = [entry firstMatchInString:src options:0 range:NSMakeRange(0, src.length)].range;
    if (e.location == NSNotFound) return nil;
    NSUInteger close = nvn_fbread_close(src, e);
    if (close == NSNotFound) return nil;
    NSString *params = [src substringWithRange:NSMakeRange(NSMaxRange(e), close - NSMaxRange(e))];
    NSTextCheckingResult *pm = [[NSRegularExpression regularExpressionWithPattern:@"float4\\s+(\\w+)\\s*\\[\\[position\\]\\]" options:0 error:nil] firstMatchInString:params options:0 range:NSMakeRange(0, params.length)];
    NSString *pos = pm ? [params substringWithRange:[pm rangeAtIndex:1]] : @"totk_fb_pos";
    NSMutableString *add = [NSMutableString stringWithString:pm ? @"" : @", float4 totk_fb_pos [[position]]"];
    NSString *out = src; unsigned have = 0;
    for (int i = 0; i < n; i++) {
        if (att[i] < 0 || att[i] > 7) return nil;
        NSString *decl = [NSString stringWithFormat:@"texture2d<float>\\s+(\\w+)\\s*\\[\\[id\\(%d\\)\\]\\]", aid[i]];
        NSTextCheckingResult *m = [[NSRegularExpression regularExpressionWithPattern:decl options:0 error:nil] firstMatchInString:out options:0 range:NSMakeRange(0, out.length)];
        if (!m) return nil;
        NSString *t = [out substringWithRange:[m rangeAtIndex:1]];
        NSRegularExpression *use = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"spvDescriptorSet0\\.%@\\b(?!Smplr)", t] options:0 error:nil];
        NSRegularExpression *smp = [NSRegularExpression regularExpressionWithPattern:[NSString stringWithFormat:@"spvDescriptorSet0\\.%@\\.sample\\((\\s*spvDescriptorSet0\\.%@Smplr\\s*,)", t, t] options:0 error:nil];
        NSUInteger uses = [use numberOfMatchesInString:out options:0 range:NSMakeRange(0, out.length)], samples = [smp numberOfMatchesInString:out options:0 range:NSMakeRange(0, out.length)];
        if (!samples || samples != uses) return nil;   // read / gather / size queries keep reading memory: leave the shader alone
        out = [smp stringByReplacingMatchesInString:out options:0 range:NSMakeRange(0, out.length)
                                       withTemplate:[NSString stringWithFormat:@"totk_fb_sample(totk_fb_in%d, %@, spvDescriptorSet0.%@,$1", att[i], pos, t]];
        if (!(have >> att[i] & 1)) { [add appendFormat:@", float4 totk_fb_in%d [[color(%d)]]", att[i], att[i]]; have |= 1u << att[i]; }
    }
    NSRange r = [out rangeOfString:@"using namespace metal;\n"];
    e = [entry firstMatchInString:out options:0 range:NSMakeRange(0, out.length)].range;
    if (r.location == NSNotFound || e.location == NSNotFound) return nil;
    close = nvn_fbread_close(out, e);
    if (close == NSNotFound) return nil;
    out = [out stringByReplacingCharactersInRange:NSMakeRange(close, 0) withString:add];
    NSString *helper =   // nearest texel of uv; the attachment has one level, so a level or bias argument changes nothing
        @"template<typename... A> static inline float4 totk_fb_sample(float4 fb, float4 pos, texture2d<float> t, sampler s, float2 uv, A... a)\n"
        @"{ return all(int2(floor(uv * float2(t.get_width(), t.get_height()))) == int2(pos.xy)) ? fb : t.sample(s, uv, a...); }\n";
    return [out stringByReplacingCharactersInRange:NSMakeRange(NSMaxRange(r), 0) withString:helper];
}
