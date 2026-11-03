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
// The file holds only what still has to be sent (user 2026-11-01: every issue used to carry the whole file, 83% of its
// shaders and 99% of its pipelines already in the list): before the reminder counts it, rows the app's own seed and warm
// lists serve leave it (totk_contrib_prune, once per app), and rows sent automatically leave it after the issue is made
// (contrib_submit.h). Both go to a hidden done file beside it, which the game and ios/tools/shader_contrib_watch.py read
// as rows they have, so neither writes them again.
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef TOTK_CONTRIB_STEP
#define TOTK_CONTRIB_STEP 100   // rows of both kinds (user 2026-10-22; 500 shaders only before)
#endif
#ifndef GAME_CONTRIB_TEMPLATE   /* the issue form, label and title prefix of this game's list (runtime/game_version.h sets Breath of the Wild's) */
#define GAME_CONTRIB_TEMPLATE "shader-list.yml"
#define GAME_CONTRIB_TITLE "Shader list:"   /* what the Mac reminder's automatic submission titles its issue with; collect_shader_issues.py finds it by this */
#endif
#define TOTK_CONTRIB_REPO "ly05010419/MacNativeGames"   // the Mac reminder's "Submit automatically" (gh issue create)
#define TOTK_CONTRIB_ISSUE_URL "https://github.com/ly05010419/MacNativeGames/issues/new?template=" GAME_CONTRIB_TEMPLATE
#define TOTK_REPO_URL "https://github.com/" TOTK_CONTRIB_REPO   // the Mac reminder's star button: the page, where the player stars it

/* Nothing is written without the seed (TOTK_SHADER_SEED=off, a test of a first run): every variant is translated in play
   then, so each would look new to the list (one such session wrote 1,670 false rows and set off the reminder). */
static inline NSString *totk_contrib_path(void) {
    const char *e = getenv("TOTK_SHADER_CONTRIB"), *seed = getenv("TOTK_SHADER_SEED");
    if (seed && !strcmp(seed, "off")) return nil;
    return e && *e ? @(e) : nil;
}

/* A row of the file, a whole line: "V\t<shader>\t<deps>" per shader and bind state, "R\t<pipeline key>",
   "B\t<pipeline key>\t<vertex>\t<fragment>" and "T\t<pipeline key>\t<tile reads>" per render pipeline (shader_list.py
   rows). T rows were not read back before 2026-11-01, so each session wrote them again (#1454: 79 lines, 41 rows). */
static inline BOOL totk_contrib_is_row(NSString *line) {
    return [line hasPrefix:@"V\t"] || [line hasPrefix:@"R\t"] || [line hasPrefix:@"B\t"] || [line hasPrefix:@"T\t"];
}
static inline NSMutableSet<NSString *> *totk_contrib_read(NSString *path) {
    NSMutableSet *rows = [NSMutableSet set];
    NSString *text = path ? [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] : nil;
    for (NSString *line in [text componentsSeparatedByString:@"\n"])
        if (totk_contrib_is_row(line)) [rows addObject:line];
    return rows;
}

/* A hidden file beside the contribution file, .<name>.<ext>, so a player's folder shows only the files to send (the project
   folder's shader-list-contribution/): "reminded" (the last reminder's row count or "never"), "done" (rows sent or served
   by the app: never written again), "pruned" (the app the file was last checked against). */
static inline NSString *totk_contrib_side(NSString *path, NSString *ext) {
    return [path.stringByDeletingLastPathComponent stringByAppendingPathComponent:[NSString stringWithFormat:@".%@.%@", path.lastPathComponent, ext]];
}
static inline NSString *totk_contrib_state(NSString *path) { return totk_contrib_side(path, @"reminded"); }
/* The reminder is due when the file holds TOTK_CONTRIB_STEP rows more than at the last one (<file>.reminded), unless the
   player chose "Don't remind me again" ("never" there). Returns the row count then, else 0. */
static inline NSUInteger totk_contrib_due(NSString *path) {
    if (!path) return 0;
    NSString *last = [NSString stringWithContentsOfFile:totk_contrib_state(path) encoding:NSUTF8StringEncoding error:nil];
    if ([last hasPrefix:@"never"]) return 0;
    NSUInteger rows = totk_contrib_read(path).count;
    NSUInteger at = last ? (NSUInteger)strtoull(last.UTF8String, NULL, 10) : 0;
    return rows >= at + TOTK_CONTRIB_STEP ? rows : 0;
}
static inline void totk_contrib_reminded(NSString *path, NSUInteger rows) {
    [[NSString stringWithFormat:@"%lu\n", (unsigned long)rows] writeToFile:totk_contrib_state(path)
                                                                 atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
/* "Don't remind me again": no more reminders for this file. The game still notes the shaders. */
static inline void totk_contrib_never(NSString *path) {
    [@"never\n" writeToFile:totk_contrib_state(path) atomically:YES encoding:NSUTF8StringEncoding error:nil];
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
/* Appends one row (a whole line without its newline), once: not when the file or its done file has it. Not thread-safe:
   the runtime calls it under its translation lock; the file's lock (flock) keeps it apart from totk_contrib_retire in
   another process (the launcher, shader_contrib_watch.py). TOTK_CONTRIB_GAME (the iPhone / iPad app: "TOTK 1.4.3") adds a
   "# Game: " line, once per file and game, so the Mac that sends a device's file names the game and version in the issue
   (ios/tools/shader_contrib_watch.py). */
static inline void totk_contrib_add_line(NSString *line) {
    static NSString *path, *gameLine; static NSMutableSet<NSString *> *rows; static int off, tagged;
    if (off) return;
    if (!rows) {
        path = totk_contrib_path();
        if (!path) { off = 1; return; }
        rows = totk_contrib_read(path);
        [rows unionSet:totk_contrib_read(totk_contrib_side(path, @"done"))];
        const char *game = getenv("TOTK_CONTRIB_GAME");
        if (game && *game && !strchr(game, '\n')) {
            gameLine = [NSString stringWithFormat:@"# Game: %s", game];
            NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            tagged = text && [[text componentsSeparatedByString:@"\n"] containsObject:gameLine];
        } else tagged = 1;
    }
    if ([rows containsObject:line]) return;
    int fd = totk_contrib_lock(path, O_WRONLY | O_APPEND | O_CREAT);
    FILE *f = fd < 0 ? NULL : fdopen(fd, "a");
    if (!f) { if (fd >= 0) close(fd); return; }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0)
        fputs("# Shaders and render pipelines this game met that the shared shader list does not have yet (docs/shader-list.md).\n"
              "# Only shader keys, bind states and pipeline descriptors: no game data, saves or personal information.\n"
              "# Send it in a GitHub issue: " TOTK_CONTRIB_ISSUE_URL "\n", f);
    if (!tagged) { fprintf(f, "%s\n", gameLine.UTF8String); tagged = 1; }   /* an older file, or another version since */
    fprintf(f, "%s\n", line.UTF8String);
    fclose(f);   // the lock goes with the descriptor
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
/* The bind state a list row of these deps is translated with: prepare_shader_seed.py answers_for (no answers for a kind
   but the deps', "*" / "*_depth" as 2D textures). */
static inline NSDictionary *totk_contrib_answers(NSDictionary *deps) {
    NSMutableDictionary *answers = [NSMutableDictionary dictionary];
    for (NSString *kind in @[@"tex", @"img", @"attr", @"fout"])
        answers[kind] = [deps[kind] isKindOfClass:[NSDictionary class]] ? deps[kind] : @{};
    NSMutableDictionary *tex = [NSMutableDictionary dictionary];
    for (NSString *slot in answers[@"tex"]) {
        id kind = answers[@"tex"][slot];
        tex[slot] = [kind isEqual:@"*"] ? @"2d" : [kind isEqual:@"*_depth"] ? @"2d_depth" : kind;
    }
    answers[@"tex"] = tex;
    return answers;
}
/* The name a build gives the variant it translates for a list row of this shader and deps: prepare_shader_seed.py
   key + "-" + suffix(answers_for(deps)). Pipeline rows name their shaders so, since a variant translated on the spot is
   named after this Mac's answers instead. */
static inline NSString *totk_contrib_listed_name(NSString *shader, NSDictionary *deps) {
    NSMutableString *json = [NSMutableString string];
    totk_contrib_json(json, totk_contrib_answers(deps));
    NSData *d = [json dataUsingEncoding:NSUTF8StringEncoding];
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, md);
    return [NSString stringWithFormat:@"%@-%02x%02x%02x%02x%02x%02x", shader, md[0], md[1], md[2], md[3], md[4], md[5]];
}

/* Whether a variant translated with deps serves these answers. A texture slot whose kind the translator took from the
   sampling instruction records only whether a depth-format texture sits there: "*" (any other) or "*_depth"
   (Service.cs, docs/shader-variants.md). Vertex input and colour output types do not matter: the pipeline redeclares an
   input whose type differs from its bound format's (nvn_mtl.m vs_vin, 2026-10-25) and an output whose type differs from
   its render target's (fn_retype_outputs, 2026-10-26), and an unbound input gets a constant of the declared type
   (pso_desc), so any variant serves them. The runtime picks variants with it (totk_shader_rt.m); prepare_shader_seed.py
   deps_match is the same. */
static inline int totk_deps_match(NSDictionary *deps, NSDictionary *ans) {
    NSDictionary *defaults = @{@"tex": @"2d", @"img": @{}};
    for (NSString *kind in defaults) {
        NSDictionary *want = deps[kind], *have = ans[kind];
        for (NSString *slot in want) {
            id w = want[slot], h = have[slot];
            if (!h) h = defaults[kind];
            if ([w isEqual:@"*"] || [w isEqual:@"*_depth"]) {
                if (![h isKindOfClass:NSString.class] || [h hasSuffix:@"_depth"] != [w isEqual:@"*_depth"]) return 0;
                continue;
            }
            if (![h isEqual:w]) return 0;
        }
    }
    return 1;
}

/* A warm list's line (pso_title.tsv, pso_manifest.tsv, iOS's pso_list.tsv) as the contribution row it stands for: an R
   row by its key, a B or T row whole; nil for any other line. nvn_mtl.m pso_contribute leaves out the rows the lists have
   by it, and totk_contrib_prune drops them by it. */
static inline NSString *totk_contrib_list_row(NSString *line) {
    NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];
    if (f.count >= 2 && [f[0] isEqualToString:@"R"]) return [@"R\t" stringByAppendingString:f[1]];
    if ((f.count >= 4 && [f[0] isEqualToString:@"B"]) || (f.count >= 3 && [f[0] isEqualToString:@"T"])) return line;
    return nil;
}

/* Moves these rows from the file to its done file, under the file's lock: header and game lines and every other row stay,
   in order; the file is replaced by a rename (totk_contrib_lock). The reminder's count comes down by the rows moved
   ("never" stays), so the next reminder counts the rows left. Returns how many moved; 0 when the done file could not be
   written (the file is left as it was). */
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
                 rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation)) unlink(tmp.fileSystemRepresentation);   // rows in both: harmless
    }
    if (count) {
        NSString *last = [NSString stringWithContentsOfFile:totk_contrib_state(path) encoding:NSUTF8StringEncoding error:nil];
        if (last && ![last hasPrefix:@"never"]) {
            NSUInteger at = (NSUInteger)strtoull(last.UTF8String, NULL, 10);
            totk_contrib_reminded(path, at > count ? at - count : 0);
        }
    }
    close(fd);
    return count;
}

/* The app the file is checked against, by the files that say what it serves: its seed's manifest and its warm lists
   (size and time, which a new build changes); nil without a seed. */
static inline NSString *totk_contrib_app(NSString *seedDir, NSString *listDir) {
    NSMutableString *app = [NSMutableString string];
    NSArray *files = @[[seedDir stringByAppendingPathComponent:@"seed-manifest.json"], [listDir stringByAppendingPathComponent:@"pso_title.tsv"],
                       [listDir stringByAppendingPathComponent:@"pso_manifest.tsv"], [listDir stringByAppendingPathComponent:@"pso_list.tsv"]];
    for (NSUInteger i = 0; i < files.count; i++) {
        struct stat st;
        if (stat([files[i] fileSystemRepresentation], &st)) { if (!i) return nil; [app appendString:@"-\n"]; continue; }
        [app appendFormat:@"%lld %ld.%09ld\n", (long long)st.st_size, (long)st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec];
    }
    return app;
}

/* The seed's variants of a shader (<shader>.variants: [{variant, deps}], as totk_shader_rt.m variants_of reads them), or
   nil when the seed has none (another game version's shader, or one it could not translate); cached by shader. */
static inline NSArray<NSDictionary *> *totk_contrib_seed_variants(NSMutableDictionary *cache, NSString *seedDir, NSString *shader) {
    id vs = cache[shader];
    if (!vs) {
        NSData *d = [NSData dataWithContentsOfFile:[seedDir stringByAppendingPathComponent:[shader stringByAppendingString:@".variants"]]];
        NSMutableArray *rows = [NSMutableArray array];
        id parsed = d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil;
        for (id v in [parsed isKindOfClass:NSArray.class] ? parsed : @[])
            if ([v isKindOfClass:NSDictionary.class] && [v[@"variant"] isKindOfClass:NSString.class]) [rows addObject:v];
        cache[shader] = vs = rows.count ? rows : NSNull.null;
    }
    return vs == NSNull.null ? nil : vs;
}
static inline NSDictionary *totk_contrib_deps_of(NSDictionary *v) { return [v[@"deps"] isKindOfClass:NSDictionary.class] ? v[@"deps"] : @{}; }

/* The name the app's warm lists give "<shader>-<suffix>_b" in a pipeline key, as shader_list.py resolve names them
   (served): itself when the seed has that variant, the seed's variant of the same suffix (another shader's translations
   serve it), else the first seed variant serving the bind state behind the name, which the V row the game writes with
   every such name gives (states: name -> deps; the list's A rows for resolve). nil when the seed cannot say. */
static inline NSString *totk_contrib_seed_name(NSString *name, NSArray<NSDictionary *> *vs, NSDictionary<NSString *, NSDictionary *> *states) {
    static NSCharacterSet *notHex;
    if (!notHex) notHex = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet;
    BOOL shaped = name.length == 79 && [name hasSuffix:@"_b"] && [name characterAtIndex:64] == '-' &&
                  [[name substringToIndex:64] rangeOfCharacterFromSet:notHex].location == NSNotFound &&
                  [[name substringWithRange:NSMakeRange(65, 12)] rangeOfCharacterFromSet:notHex].location == NSNotFound;
    if (!shaped) return name;   // no variant name (a hand-made row): as it is
    if (!vs) return nil;
    NSString *variant = [name substringToIndex:77], *suffix = [variant substringFromIndex:64];
    for (NSDictionary *v in vs) if ([v[@"variant"] isEqualToString:variant]) return name;
    for (NSDictionary *v in vs) if ([v[@"variant"] hasSuffix:suffix]) return [v[@"variant"] stringByAppendingString:@"_b"];
    NSDictionary *deps = states[name];
    if (!deps || [suffix isEqualToString:totk_contrib_listed_name(@"", @{})]) return nil;   // the default state the seed lacks
    NSDictionary *answers = totk_contrib_answers(deps);
    for (NSDictionary *v in vs) if (totk_deps_match(totk_contrib_deps_of(v), answers)) return [v[@"variant"] stringByAppendingString:@"_b"];
    return nil;
}

/* A pipeline row (R / B / T) with the two shaders its key names (nvn_pso_skey.h pso_skey: char vs[80], fs[80] lead the
   544 bytes, hex here) renamed as the app's warm lists name them (shader_list.py rename_warm); nil when a name cannot be
   told or the key is not one. */
static inline NSString *totk_contrib_seed_row(NSString *row, NSMutableDictionary *cache, NSString *seedDir, NSDictionary *states) {
    enum { NAME = 80 };
    NSMutableArray<NSString *> *f = [[row componentsSeparatedByString:@"\t"] mutableCopy];
    if (f.count < 2 || f[1].length < 4 * NAME) return nil;
    NSMutableString *hex = [f[1] mutableCopy];
    for (NSUInteger at = 0; at < 2; at++) {
        char bytes[NAME + 1] = {0};
        for (NSUInteger i = 0; i < NAME; i++) {
            unsigned b; if (sscanf([[hex substringWithRange:NSMakeRange((at * NAME + i) * 2, 2)] UTF8String], "%2x", &b) != 1) return nil;
            bytes[i] = (char)b;
        }
        NSString *old = @(bytes);
        if (!old.length) continue;
        NSString *shader = old.length >= 64 ? [old substringToIndex:64] : old;
        NSString *mine = totk_contrib_seed_name(old, totk_contrib_seed_variants(cache, seedDir, shader), states);
        if (!mine || mine.length >= NAME) return nil;
        if ([mine isEqualToString:old]) continue;
        NSMutableString *put = [NSMutableString string];
        for (NSUInteger i = 0; i < NAME; i++) [put appendFormat:@"%02x", i < mine.length ? [mine characterAtIndex:i] : 0];
        [hex replaceCharactersInRange:NSMakeRange(at * NAME * 2, NAME * 2) withString:put];
    }
    f[1] = hex;
    return [f componentsJoinedByString:@"\t"];
}

/* Rows the app serves leave the file for its done file, once per app (the "pruned" side file): a V row whose bind state a
   variant of the app's seed serves (picked as the runtime picks one), an R / B / T row the app's warm lists hold, as it
   is or with its shaders named as the lists name them (totk_contrib_seed_row; 18,092 of the 26,628 rows of a 1.4.3
   build's pso_manifest.tsv are renamed so). Such rows were written by an older app, or another player's file brought
   them into the list since; the game would not write them now, and an issue carrying them adds nothing. A shader the
   seed does not have (another game version's) and a name the seed cannot tell stay: only what is certainly there
   leaves. Called by the Mac launcher before its reminder counts the file and by the iPhone / iPad app before the game
   starts. Returns the rows moved. */
static inline NSUInteger totk_contrib_prune(NSString *path, NSString *seedDir, NSString *listDir) {
    NSString *app = path ? totk_contrib_app(seedDir, listDir) : nil, *mark = path ? totk_contrib_side(path, @"pruned") : nil;
    if (!app || [[NSString stringWithContentsOfFile:mark encoding:NSUTF8StringEncoding error:nil] isEqualToString:app]) return 0;
    NSMutableSet<NSString *> *served = [NSMutableSet set];
    NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *pipes = [NSMutableDictionary dictionary];   // list form -> the rows (one
    // with this build's names and one with the portable ones become the same)
    NSMutableDictionary *seed = [NSMutableDictionary dictionary];
    NSMutableDictionary<NSString *, NSDictionary *> *states = [NSMutableDictionary dictionary];   // portable name -> deps
    NSMutableSet<NSString *> *rows = totk_contrib_read(path), *known = [rows mutableCopy];
    [known unionSet:totk_contrib_read(totk_contrib_side(path, @"done"))];   // a V row moved before still names its state
    for (NSString *row in known) @autoreleasepool {
        NSArray<NSString *> *f = [row hasPrefix:@"V\t"] ? [row componentsSeparatedByString:@"\t"] : nil;
        id deps = f.count == 3 ? [NSJSONSerialization JSONObjectWithData:[f[2] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] : nil;
        if ([deps isKindOfClass:NSDictionary.class]) states[[totk_contrib_listed_name(f[1], deps) stringByAppendingString:@"_b"]] = deps;
    }
    for (NSString *row in rows) @autoreleasepool {
        if (![row hasPrefix:@"V\t"]) {
            NSString *form = totk_contrib_list_row(row), *renamed = form ? totk_contrib_seed_row(form, seed, seedDir, states) : nil;
            for (NSString *as in @[form ?: @"", renamed ?: @""]) if (as.length) [pipes[as] ?: (pipes[as] = [NSMutableSet set]) addObject:row];
            continue;
        }
        NSArray<NSString *> *f = [row componentsSeparatedByString:@"\t"];
        NSArray *vs = f.count == 3 ? totk_contrib_seed_variants(seed, seedDir, f[1]) : nil;
        id deps = vs ? [NSJSONSerialization JSONObjectWithData:[f[2] dataUsingEncoding:NSUTF8StringEncoding] options:0 error:nil] : nil;
        if (![deps isKindOfClass:NSDictionary.class]) continue;
        NSDictionary *answers = totk_contrib_answers(deps);
        for (NSDictionary *v in vs) if (totk_deps_match(totk_contrib_deps_of(v), answers)) { [served addObject:row]; break; }
    }
    for (NSString *name in pipes.count ? @[@"pso_title.tsv", @"pso_manifest.tsv", @"pso_list.tsv"] : @[]) {
        FILE *f = fopen([listDir stringByAppendingPathComponent:name].fileSystemRepresentation, "r");
        char *line = NULL; size_t cap = 0; ssize_t len;
        while (f && (len = getline(&line, &cap, f)) > 0) @autoreleasepool {
            if (line[0] != 'R' && line[0] != 'B' && line[0] != 'T') continue;
            if (line[len - 1] == '\n') line[--len] = 0;
            NSString *form = totk_contrib_list_row([[NSString alloc] initWithBytes:line length:(NSUInteger)len encoding:NSUTF8StringEncoding] ?: @"");
            NSSet *those = form ? pipes[form] : nil;
            if (those) [served unionSet:those];
        }
        free(line); if (f) fclose(f);
    }
    NSUInteger moved = totk_contrib_retire(path, served);
    if (moved == served.count) [app writeToFile:mark atomically:YES encoding:NSUTF8StringEncoding error:nil];   // else again next time
    return moved;
}
