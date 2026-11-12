// TOTK: what a player's game adds to the shared shader list (docs/shader-list.md).
// The prepared seed holds every variant the list names, so a variant the Mac has to translate while playing, or that an
// iPhone / iPad misses, is one the list does not have yet. With a seed that has every shader's default variant
// (all_default), only states those variants do not serve are written (totk_shader_rt.m contrib_wanted): the list keeps
// non-default variants. Render pipelines the game builds in play that the app's warm-up lists lack are written too
// (nvn_mtl.m pso_contribute; user 2026-10-22), their shaders named as a build names them (totk_contrib_listed_name).
// Each row is written once to the player's contribution file (TOTK_SHADER_CONTRIB, set by app/launcher.m and
// ios/Sources/main.m; unset in development runs, which then write nothing) as a shader-list V, R or B row: shader keys,
// bind states and pipeline descriptors, no game data. Once the file holds TOTK_CONTRIB_STEP new rows, the
// Mac launcher asks once, before the game starts, whether to contribute them automatically over HTTPS
// (totk_contrib_upload.h; a switch in Settings changes it later); games/totk/scripts/collect_shaders.py --inbox merges what
// arrives. Header-only: the runtime and the Mac launcher share it.
#pragma once
#import <Foundation/Foundation.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef TOTK_CONTRIB_STEP   /* runtime/game_version.h keeps Breath of the Wild's at 500 */
#define TOTK_CONTRIB_STEP 100   // rows of both kinds (user 2026-10-22; 500 shaders only before)
#endif
/* This game's own shader list, games/ssbu/assets/shader_list.tsv (user 2026-10-28: every game its own list, never mixed
   into Tears of the Kingdom's): a contribution file named after it, uploaded under
   the game's inbox id (GAME_INBOX_ID, totk_contrib_upload.h). */
#define GAME_CONTRIB_FILE "shader-list-contribution-ssbu.txt"

/* Nothing is written without the seed (TOTK_SHADER_SEED=off, a test of a first run): every variant is translated in play
   then, so each would look new to the list (one such session wrote 1,670 false rows and set off the reminder). */
static inline NSString *totk_contrib_path(void) {
    const char *e = getenv("TOTK_SHADER_CONTRIB"), *seed = getenv("TOTK_SHADER_SEED");
    if (seed && !strcmp(seed, "off")) return nil;
    return e && *e ? @(e) : nil;
}

/* A row of the file, a whole line (shader-list rows): what is counted, sent and moved to the done file. */
static inline BOOL totk_contrib_is_row(NSString *line) {
    return [line hasPrefix:@"V\t"] || [line hasPrefix:@"R\t"] || [line hasPrefix:@"B\t"] || [line hasPrefix:@"T\t"];
}
/* The rows already in the file, whole lines: "V\t<shader>\t<deps>" per shader and bind state, "R\t<pipeline key>" and
   "B\t<pipeline key>\t<vertex>\t<fragment>" per render pipeline (shader_list.py rows). */
static inline NSMutableSet<NSString *> *totk_contrib_read(NSString *path) {
    NSMutableSet *rows = [NSMutableSet set];
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        if (totk_contrib_is_row(line)) [rows addObject:line];
    return rows;
}

/* A hidden file beside the contribution file, .<name>.<ext> (the shader inbox port, 2026-11-10): "done" holds the rows the
   inbox took (never written or sent again), "upload" the player's choice, "install" a random id. */
static inline NSString *totk_contrib_side(NSString *path, NSString *ext) {
    return [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.%@", path.lastPathComponent, ext]];
}
/* The game a contribution file belongs to: its line "# Inbox game: <id>" (the shader inbox's id for the game, GAME_INBOX_ID),
   or nil. Every game's shaders stay apart from writing to sending (user 2026-11-12: "每个游戏的着色器都独立"): the launcher
   claims the file before the game starts (totk_contrib_upload.h totk_inbox_claim) and gives the game the id in
   TOTK_CONTRIB_GAME_ID; the game writes only to a file of its own id (totk_contrib_writable) and the upload sends only such a
   file (totk_inbox_send). Before, a file could hold several games' rows and the app that sent it named its own game for all
   of them (2026-11-11: an Ocarina of Time install sent 418 Tears of the Kingdom pipelines; 2026-11-12: 464 Pokopia rows came
   as Breath of the Wild's, the two games sharing one file). */
#define TOTK_CONTRIB_OWNER "# Inbox game: "
static inline NSString *totk_contrib_owner(NSString *path) {
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        if ([line hasPrefix:@TOTK_CONTRIB_OWNER]) return [line substringFromIndex:strlen(TOTK_CONTRIB_OWNER)];
    return nil;
}
/* This game's id as the launcher's claim gave it (TOTK_CONTRIB_GAME_ID), or nil (a development run: no check). */
static inline NSString *totk_contrib_writer_id(void) {
    const char *id = getenv("TOTK_CONTRIB_GAME_ID");
    return id && *id && !strchr(id, '\n') ? @(id) : nil;
}
/* Whether this game may write to the file: it is this game's, or not there yet (the first row names the game). */
static inline BOOL totk_contrib_writable(NSString *path) {
    NSString *id = totk_contrib_writer_id();
    if (!id || !path) return YES;
    NSString *found = [NSFileManager.defaultManager fileExistsAtPath:path] ? totk_contrib_owner(path) : id;
    if ([found isEqualToString:id]) return YES;
    fprintf(stderr, "[shader-contrib] %s is %s, not %s's: nothing is written to it\n", path.UTF8String,
            found.length ? [NSString stringWithFormat:@"%@'s", found].UTF8String : "no game's", id.UTF8String);
    return NO;
}

/* The file opened with these flags and locked (flock, exclusive), or -1. totk_contrib_retire replaces the file by a rename,
   so a descriptor opened before it names the old one: then the file is opened again. */
static inline int totk_contrib_lock(NSString *path, int flags) {
    for (int tries = 0; tries < 8; tries++) {
        int fd = open(path.fileSystemRepresentation, flags | O_CLOEXEC, 0644);
        if (fd < 0) return -1;
        struct stat held, now;
        if (flock(fd, LOCK_EX) == 0 && fstat(fd, &held) == 0 && stat(path.fileSystemRepresentation, &now) == 0 &&
            held.st_ino == now.st_ino && held.st_dev == now.st_dev) return fd;
        close(fd);
    }
    return -1;
}
/* Moves these rows from the file to its done file, under the file's lock: header lines and every other row stay, in
   order; the file is replaced by a rename. Returns how many moved; 0 when the done file could not be written. */
static inline NSUInteger totk_contrib_retire(NSString *path, NSSet<NSString *> *rows) {
    if (!path || !rows.count) return 0;
    int fd = totk_contrib_lock(path, O_RDONLY);
    if (fd < 0) return 0;
    NSMutableData *data = [NSMutableData data]; char buf[1 << 16]; ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) [data appendBytes:buf length:(NSUInteger)n];
    NSString *text = n < 0 ? nil : [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    NSMutableString *kept = [NSMutableString string], *moved = [NSMutableString string];
    NSMutableSet *seen = [NSMutableSet set];
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    for (NSUInteger i = 0; i < lines.count; i++) {
        NSString *line = lines[i];
        if (i + 1 == lines.count && !line.length) break;   // after the last newline
        if (!totk_contrib_is_row(line) || ![rows containsObject:line]) { [kept appendFormat:@"%@\n", line]; continue; }
        if (![seen containsObject:line]) { [seen addObject:line]; [moved appendFormat:@"%@\n", line]; }
    }
    NSUInteger count = seen.count;
    if (count) {
        NSString *done = totk_contrib_side(path, @"done"), *tmp = totk_contrib_side(path, @"tmp");
        FILE *f = fopen(done.fileSystemRepresentation, "a");
        BOOL wrote = f && fputs(moved.UTF8String, f) >= 0;
        if (f && fclose(f)) wrote = NO;
        if (!wrote) count = 0;
        else if (![kept writeToFile:tmp atomically:NO encoding:NSUTF8StringEncoding error:nil] ||
                 rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation)) unlink(tmp.fileSystemRepresentation);
    }
    close(fd);
    return count;
}

/* Where the reminder's state is kept, <file>.reminded (this game's place for it; totk_contrib_upload.h reads the player's choice
   there). totk_contrib_upload.h called it since 9e212f52 without this copy having it, so the launcher did not build. */
static inline NSString *totk_contrib_state(NSString *path) { return [path stringByAppendingString:@".reminded"]; }
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
        if (!path || !totk_contrib_writable(path)) { off = 1; return; }
        rows = totk_contrib_read(path);
        [rows unionSet:totk_contrib_read(totk_contrib_side(path, @"done"))];   /* rows the inbox took */
    }
    if ([rows containsObject:line]) return;
    int fd = totk_contrib_lock(path, O_WRONLY | O_APPEND | O_CREAT);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "a");
    if (!f) { if (fd >= 0) close(fd); return; }
    fseek(f, 0, SEEK_END);
    int fresh = ftell(f) == 0;
    if (fresh)
        fputs("# Shaders and render pipelines this game met that the shared shader list does not have yet (docs/shader-list.md).\n"
              "# Only shader keys, bind states and pipeline descriptors: no game data, saves or personal information.\n"
              "# This game sends it to the shader inbox when contributing is on in Settings (docs/shader-list.md).\n", f);
    if (fresh && totk_contrib_writer_id()) fprintf(f, TOTK_CONTRIB_OWNER "%s\n", totk_contrib_writer_id().UTF8String);
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
