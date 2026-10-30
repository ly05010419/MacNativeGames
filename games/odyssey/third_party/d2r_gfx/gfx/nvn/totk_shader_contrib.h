// TOTK: what a player's game adds to the shared shader list (docs/shader-list.md).
// The prepared seed holds every variant the list names, so a variant the Mac has to translate while playing, or that an
// iPhone / iPad misses, is one the list does not have yet. Such a variant is written once to the player's contribution
// file (TOTK_SHADER_CONTRIB, set by app/launcher.m and ios/Sources/main.m; unset in development runs, which then write
// nothing) as a shader-list V row: the shader key and the bind state, no game data. Every TOTK_CONTRIB_STEP new rows the
// Mac launcher asks the player, before the game starts, to send the file in a GitHub issue (an iPhone / iPad never asks
// during play: while it is connected, ios/tools/shader_contrib_watch.py asks on the Mac); scripts/collect_shader_issues.py
// merges what arrives. Header-only: the runtime and the Mac launcher share it.
#pragma once
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOTK_CONTRIB_STEP 500
/* This game's own shader list, games/odyssey/assets/shader_list.tsv (user 2026-10-28: every game its own list, never mixed
   into Tears of the Kingdom's): the issue form of the other games with the game and the title filled in, an automatic
   submission titled with the game (games/totk/scripts/collect_shader_issues.py OTHER finds it by that title), and a
   contribution file named after it. */
#define GAME_CONTRIB_TEMPLATE "shader-list-other.yml"
#define GAME_CONTRIB_GAME "Super Mario Odyssey"
#define GAME_CONTRIB_TITLE "Shader list (Super Mario Odyssey):"
#define GAME_CONTRIB_FILE "shader-list-contribution-odyssey.txt"
#define TOTK_CONTRIB_REPO "ly05010419/MacNativeGames"   // the Mac reminder's "Submit automatically" (gh issue create)
#define TOTK_CONTRIB_ISSUE_URL "https://github.com/ly05010419/MacNativeGames/issues/new?template=" GAME_CONTRIB_TEMPLATE \
    "&game=Super+Mario+Odyssey&title=Shader+list+%28Super+Mario+Odyssey%29%3A+"

static inline NSString *totk_contrib_path(void) {
    const char *e = getenv("TOTK_SHADER_CONTRIB");
    return e && *e ? @(e) : nil;
}

/* The rows already in the file: "<shader>\t<deps>", one per shader and bind state. */
static inline NSMutableSet<NSString *> *totk_contrib_read(NSString *path) {
    NSMutableSet *rows = [NSMutableSet set];
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        if ([line hasPrefix:@"V\t"]) [rows addObject:[line substringFromIndex:2]];
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

/* Adds the shader with these bind-state answers or translator deps (only tex / img / attr / fout are kept), once.
   Not thread-safe: the runtime calls it under its translation lock. */
static inline void totk_contrib_add(NSString *shader, NSDictionary *deps) {
    static NSString *path; static NSMutableSet<NSString *> *rows; static int off;
    if (off) return;
    if (!rows) {
        path = totk_contrib_path();
        if (!path) { off = 1; return; }
        rows = totk_contrib_read(path);
    }
    NSMutableDictionary *kept = [NSMutableDictionary dictionary];
    for (NSString *kind in @[@"tex", @"img", @"attr", @"fout"])
        if ([deps[kind] isKindOfClass:[NSDictionary class]]) kept[kind] = deps[kind];
    NSMutableString *row = [NSMutableString stringWithFormat:@"%@\t", shader];
    totk_contrib_json(row, kept);
    if ([rows containsObject:row]) return;
    FILE *f = fopen(path.fileSystemRepresentation, "a");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fputs("# Shaders this game met that the shared shader list does not have yet (docs/shader-list.md).\n"
              "# Only shader keys and bind states: no game data, saves or personal information.\n"
              "# Send it in a GitHub issue: " TOTK_CONTRIB_ISSUE_URL "\n", f);
    fprintf(f, "V\t%s\n", row.UTF8String);
    fclose(f);
    [rows addObject:row];
}
