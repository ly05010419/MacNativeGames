// iPhone / iPad: the shader translator inside the app. maxmetal's library (https://github.com/svich/maxmetal, the commit
// build_app.py MAXMETAL pins; ios/project.yml compiles its src/translate.c) answers totk_shader_rt.m's translation
// requests in the process, where the Mac starts a translation service, and writes the files that service writes
// (maxmetal's cli/maxmetal.c handle, the same steps): <out>/<variant>_b.metal and <variant>_b.json. The reflection names
// the app's chain: the device's own translations go into its cache beside the seed's, as fill_shaders.py's do.
#ifndef TOTK_MAXMETAL_RT_H
#define TOTK_MAXMETAL_RT_H
#import <Foundation/Foundation.h>
#include <maxmetal/maxmetal.h>
#include <stdlib.h>
#include <string.h>

static int mm_tex_type(NSString *s, bool *depth) {
    *depth = s.length > 6 && [s hasSuffix:@"_depth"];
    NSString *b = *depth ? [s substringToIndex:s.length - 6] : s;
    NSDictionary *types = @{@"1d": @(TEXTYPE_1D), @"1darray": @(TEXTYPE_1D_ARRAY), @"2darray": @(TEXTYPE_2D_ARRAY),
                            @"3d": @(TEXTYPE_3D), @"cube": @(TEXTYPE_CUBE), @"cubearray": @(TEXTYPE_CUBE_ARRAY), @"2d": @(TEXTYPE_2D)};
    NSNumber *t = types[b];
    return t ? t.intValue : -1;   // (buffer / multisample textures: the lookup decides, or the translation fails)
}
static NSString *mm_json(id value) {   // a JSON string literal
    NSData *d = [NSJSONSerialization dataWithJSONObject:value options:NSJSONWritingFragmentsAllowed error:nil];
    return d ? [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] : @"\"\"";
}

/* One request (totk_shader_rt.m variant_request) and its guest code (graphics: from the SPH on; compute: with its 0x100-
   byte header): nil when both files are written, else why not. */
static NSString *totk_maxmetal_request(NSDictionary *rq, NSData *code, NSString *out, const char *chain) {
    NSString *variant = rq[@"variant"], *st = rq[@"stage"];
    if (![variant isKindOfClass:NSString.class] || ![st isKindOfClass:NSString.class] || !code) return @"request without variant / stage / code";
    int stage = [st isEqual:@"vs"] ? STAGE_VERTEX : [st isEqual:@"ps"] ? STAGE_FRAGMENT : [st isEqual:@"cs"] ? STAGE_COMPUTE : -1;
    if (stage < 0) return [NSString stringWithFormat:@"stage %@ not supported by the translation chain", st];
    NSArray *local = [rq[@"local"] isKindOfClass:NSArray.class] ? rq[@"local"] : nil;
    if (stage == STAGE_COMPUTE && !local.count) return @"compute shader without thread group size";
    NvnKey nk;
    memset(&nk, 0, sizeof nk);
    memset(nk.tex_type, -1, sizeof nk.tex_type);
    NSDictionary *tex = [rq[@"tex"] isKindOfClass:NSDictionary.class] ? rq[@"tex"] : @{};
    for (NSString *key in tex) {
        int slot = key.intValue; id v = tex[key];
        NSString *ts = [v isKindOfClass:NSString.class] ? v : [v isKindOfClass:NSDictionary.class] && [v[@"type"] isKindOfClass:NSString.class] ? v[@"type"] : nil;
        if (slot < 0 || slot >= 128 || !ts) continue;
        bool depth;
        nk.tex_type[slot] = (int8_t)mm_tex_type(ts, &depth);
        nk.tex_depth[slot] = depth;
    }
    nk.early_fragment_tests = [rq[@"early_fragment_tests"] boolValue];
    nk.depth_minus_w = [rq[@"depth_minus_w"] boolValue];
    nk.hw_layer_clamp = [rq[@"hw_layer_clamp"] boolValue];   // as maxmetal's service (cli/maxmetal.c handle)
    for (NSUInteger i = 0; i < local.count && i < 3; i++) nk.local[i] = [local[i] unsignedIntValue];
    // c1 (hex): the program's constants, for BRX jump tables
    NSString *c1 = [rq[@"c1"] isKindOfClass:NSString.class] ? rq[@"c1"] : nil;
    uint8_t *c1b = calloc(1, 0x10000);
    size_t n = 0;
    for (const char *h = c1.UTF8String; h && n < 0x10000 && h[2 * n] && h[2 * n + 1]; n++) {
        unsigned v;
        if (sscanf(h + 2 * n, "%2x", &v) != 1) break;
        c1b[n] = (uint8_t)v;
    }
    if (n) { nk.c1 = c1b; nk.c1_size = (uint32_t)n; }
    // The translator wants the 0x30-byte NVN header before the code (left zero), and a little room after it
    size_t size = code.length + 0x30;
    uint8_t *blob = calloc(1, size + 64);
    memcpy(blob + 0x30, code.bytes, code.length);
    const uint8_t *at = blob;
    size_t at_size = size;
    if (stage != STAGE_COMPUTE) {
        // the SPH's shader type decides the stage, as Ryujinx's ShaderHeader.Stage (a "ps" request can be a vertex program)
        uint32_t type = size >= 0x34 ? (*(const uint32_t *)(blob + 0x30) >> 10) & 0xf : 0;
        if (type != 1 && type != 5) { free(blob); free(c1b); return [NSString stringWithFormat:@"SPH shader type %u not supported", type]; }
        stage = type == 1 ? STAGE_VERTEX : STAGE_FRAGMENT;
    } else {
        for (size_t i = 0x30 + 8; i < 0x30 + 0x100 && i < size; i++)
            if (blob[i]) { free(blob); free(c1b); return [NSString stringWithFormat:@"compute code: header not as expected (byte %#zx)", i - 0x30]; }
        at = blob + 0x30 + 0x100;
        at_size = size > 0x130 ? size - 0x130 : 0;
    }
    ShaderKey key;
    memset(&key, 0, sizeof key);
    key.nvn = &nk;
    ShaderInfo *si = maxwell_translate(at, at_size, stage, &key);
    free(blob); free(c1b);
    if (!si->msl || !si->refl) { NSString *e = [NSString stringWithFormat:@"maxmetal: %s", si->error[0] ? si->error : "no output"]; maxwell_free(si); return e; }
    NSString *name = [variant stringByAppendingString:@"_b"];
    // the reflection: the request's fields around the translator's (key order as the service's)
    NSString *refl = [NSString stringWithFormat:@"{\"key\": %@, \"route\": \"B\", \"chain\": %@, \"stage\": %@, \"entry\": \"main0\", "
                      "\"msl_entry_kind\": \"%s\", \"threadgroup_size\": %@, \"c1\": %@, \"notes\": [], %s, \"raster\": {}, "
                      "\"softfloat64\": false, \"translator\": \"maxmetal\"}\n",
                      mm_json(name), mm_json(@(chain)), mm_json(st),
                      stage == STAGE_VERTEX ? "vertex" : stage == STAGE_FRAGMENT ? "fragment" : "kernel",
                      stage == STAGE_COMPUTE ? [NSString stringWithFormat:@"[%u, %u, %u]", nk.local[0], nk.local[1], nk.local[2]] : @"null",
                      mm_json(c1 ?: @""), si->refl];
    NSString *msl = @(si->msl);
    maxwell_free(si);
    NSString *base = [out stringByAppendingPathComponent:name];
    if (![msl writeToFile:[base stringByAppendingString:@".metal"] atomically:YES encoding:NSUTF8StringEncoding error:nil])
        return [NSString stringWithFormat:@"cannot write %@.metal", base];
    if (![refl writeToFile:[base stringByAppendingString:@".json"] atomically:YES encoding:NSUTF8StringEncoding error:nil])
        return [NSString stringWithFormat:@"cannot write %@.json", base];
    return nil;
}
#endif
