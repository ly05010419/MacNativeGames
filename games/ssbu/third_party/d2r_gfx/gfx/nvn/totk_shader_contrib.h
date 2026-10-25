// TOTK: what a player's game adds to the shared shader list (docs/shader-list.md).
// The prepared seed holds every variant the list names, so a variant the Mac has to translate while playing, or that an
// iPhone / iPad misses, is one the list does not have yet. With a seed that has every shader's default variant
// (all_default), only states those variants do not serve are written (totk_shader_rt.m contrib_wanted): the list keeps
// non-default variants. Render pipelines the game builds in play that the app's warm-up lists lack are written too
// (nvn_mtl.m pso_contribute; user 2026-10-22), their shaders named as a build names them (totk_contrib_listed_name).
// Each row is written once to the player's contribution file (TOTK_SHADER_CONTRIB, set by app/launcher.m and
// ios/Sources/main.m; unset in development runs, which then write nothing) as a shader-list V, R or B row: shader keys,
// bind states and pipeline descriptors, no game data. Every TOTK_CONTRIB_STEP new rows of any kind the Mac launcher asks
// the player, before the game starts, to send the file in a GitHub issue (an iPhone / iPad never asks during play: while
// it is connected, ios/tools/shader_contrib_watch.py asks on the Mac); scripts/collect_shader_issues.py merges what
// arrives. Header-only: the runtime and the Mac launcher share it.
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TOTK_CONTRIB_STEP   /* runtime/game_version.h keeps Breath of the Wild's at 500 */
#define TOTK_CONTRIB_STEP 100   // rows of both kinds (user 2026-10-22; 500 shaders only before)
#endif
#ifndef GAME_CONTRIB_TEMPLATE   /* the issue form, label and title prefix of this game's list (runtime/game_version.h sets Breath of the Wild's) */
#define GAME_CONTRIB_TEMPLATE "shader-list.yml"
#define GAME_CONTRIB_TITLE "Shader list:"   /* what the Mac reminder's automatic submission titles its issue with; collect_shader_issues.py finds it by this */
#endif
#define TOTK_CONTRIB_REPO "ly05010419/MacNativeGames"   // the Mac reminder's "Submit automatically" (gh issue create)
#define TOTK_CONTRIB_ISSUE_URL "https://github.com/ly05010419/MacNativeGames/issues/new?template=" GAME_CONTRIB_TEMPLATE

/* Nothing is written without the seed (TOTK_SHADER_SEED=off, a test of a first run): every variant is translated in play
   then, so each would look new to the list (one such session wrote 1,670 false rows and set off the reminder). */
static inline NSString *totk_contrib_path(void) {
    const char *e = getenv("TOTK_SHADER_CONTRIB"), *seed = getenv("TOTK_SHADER_SEED");
    if (seed && !strcmp(seed, "off")) return nil;
    return e && *e ? @(e) : nil;
}

/* The rows already in the file, whole lines: "V\t<shader>\t<deps>" per shader and bind state, "R\t<pipeline key>" and
   "B\t<pipeline key>\t<vertex>\t<fragment>" per render pipeline (shader_list.py rows). */
static inline NSMutableSet<NSString *> *totk_contrib_read(NSString *path) {
    NSMutableSet *rows = [NSMutableSet set];
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        if ([line hasPrefix:@"V\t"] || [line hasPrefix:@"R\t"] || [line hasPrefix:@"B\t"]) [rows addObject:line];
    return rows;
}

/* The reminder is due when the file holds TOTK_CONTRIB_STEP rows more than at the last one (<file>.reminded), unless the
   player chose "Don't remind me again" ("never" there). Returns the row count then, else 0. */
static inline NSUInteger totk_contrib_due(NSString *path) {
    if (!path) return 0;
    NSString *last = [NSString stringWithContentsOfFile:[path stringByAppendingString:@".reminded"] encoding:NSUTF8StringEncoding error:nil];
    if ([last hasPrefix:@"never"]) return 0;
    NSUInteger rows = totk_contrib_read(path).count;
    NSUInteger at = last ? (NSUInteger)strtoull(last.UTF8String, NULL, 10) : 0;
    return rows >= at + TOTK_CONTRIB_STEP ? rows : 0;
}
static inline void totk_contrib_reminded(NSString *path, NSUInteger rows) {
    [[NSString stringWithFormat:@"%lu\n", (unsigned long)rows] writeToFile:[path stringByAppendingString:@".reminded"]
                                                                 atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
/* "Don't remind me again": no more reminders for this file. The game still notes the shaders. */
static inline void totk_contrib_never(NSString *path) {
    [@"never\n" writeToFile:[path stringByAppendingString:@".reminded"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

/* Compact JSON with keys in code-point order, as scripts/shader_list.py canonical() writes it (Python's json.dumps with
   sort_keys; NSJSONWritingSortedKeys would put "9" before "10"). Deps hold only dictionaries and strings. */
static inline void totk_contrib_json(NSMutableString *out, id value) {
    if ([value isKindOfClass:[NSDictionary class]]) {
        NSArray *keys = [[value allKeys] sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) { return [a compare:b options:NSLiteralSearch]; }];
        [out appendString:@"{"];
        for (NSUInteger i = 0; i < keys.count; i++) {
            if (i) [out appendString:@","];
            totk_contrib_json(out, keys[i]); [out appendString:@":"]; totk_contrib_json(out, value[keys[i]]);
        }
        [out appendString:@"}"];
        return;
    }
    NSString *text = [value description];
    [out appendString:@"\""];
    for (NSUInteger i = 0; i < text.length; i++) {
        unichar c = [text characterAtIndex:i];
        if (c == '"' || c == '\\') [out appendFormat:@"\\%C", c];
        else if (c < 0x20 || c > 0x7f) [out appendFormat:@"\\u%04x", c];
        else [out appendFormat:@"%C", c];
    }
    [out appendString:@"\""];
}

/* Appends one row (a whole line without its newline), once. Not thread-safe: the runtime calls it under its translation
   lock. */
static inline void totk_contrib_add_line(NSString *line) {
    static NSString *path; static NSMutableSet<NSString *> *rows; static int off;
    if (off) return;
    if (!rows) {
        path = totk_contrib_path();
        if (!path) { off = 1; return; }
        rows = totk_contrib_read(path);
    }
    if ([rows containsObject:line]) return;
    FILE *f = fopen(path.fileSystemRepresentation, "a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fputs("# Shaders and render pipelines this game met that the shared shader list does not have yet (docs/shader-list.md).\n"
              "# Only shader keys, bind states and pipeline descriptors: no game data, saves or personal information.\n"
              "# Send it in a GitHub issue: " TOTK_CONTRIB_ISSUE_URL "\n", f);
    fprintf(f, "%s\n", line.UTF8String);
    fclose(f);
    [rows addObject:line];
}
/* Adds the shader with these bind-state answers or translator deps (only tex / img / attr / fout are kept), once. */
static inline void totk_contrib_add(NSString *shader, NSDictionary *deps) {
    NSMutableDictionary *kept = [NSMutableDictionary dictionary];
    for (NSString *kind in @[@"tex", @"img", @"attr", @"fout"])
        if ([deps[kind] isKindOfClass:[NSDictionary class]]) kept[kind] = deps[kind];
    NSMutableString *row = [NSMutableString stringWithFormat:@"V\t%@\t", shader];
    totk_contrib_json(row, kept);
    totk_contrib_add_line(row);
}
/* The name a build gives the variant it translates for a list row of this shader and deps: prepare_shader_seed.py
   key + "-" + suffix(answers_for(deps)) (no answers for a kind but the deps', "*" / "*_depth" as 2D textures). Pipeline
   rows name their shaders so, since a variant translated on the spot is named after this Mac's answers instead. */
static inline NSString *totk_contrib_listed_name(NSString *shader, NSDictionary *deps) {
    NSMutableDictionary *answers = [NSMutableDictionary dictionary];
    for (NSString *kind in @[@"tex", @"img", @"attr", @"fout"])
        answers[kind] = [deps[kind] isKindOfClass:[NSDictionary class]] ? deps[kind] : @{};
    NSMutableDictionary *tex = [NSMutableDictionary dictionary];
    for (NSString *slot in answers[@"tex"]) {
        id kind = answers[@"tex"][slot];
        tex[slot] = [kind isEqual:@"*"] ? @"2d" : [kind isEqual:@"*_depth"] ? @"2d_depth" : kind;
    }
    answers[@"tex"] = tex;
    NSMutableString *json = [NSMutableString string];
    totk_contrib_json(json, answers);
    NSData *d = [json dataUsingEncoding:NSUTF8StringEncoding];
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, md);
    return [NSString stringWithFormat:@"%@-%02x%02x%02x%02x%02x%02x", shader, md[0], md[1], md[2], md[3], md[4], md[5]];
}
