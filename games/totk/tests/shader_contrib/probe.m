// totk_shader_contrib.h: the contribution file and its reminder. Usage: probe <file> [noseed | game | prune]; writes the file, prints JSON.
// game: a file an older app wrote (no "# Game:" line) gets the TOTK_CONTRIB_GAME line once, before its next row.
// noseed: with TOTK_SHADER_SEED=off nothing is written (a process of its own: totk_contrib_add keeps its file once opened).
// prune: rows the app's seed and warm lists already serve, and rows sent, move to the done file and are not written again.
#import <Foundation/Foundation.h>
#include "totk_shader_contrib.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static NSString *key(unsigned i) { return [NSString stringWithFormat:@"%064x", i]; }
static void put(NSString *path, NSString *text) { [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static NSString *get(NSString *path) { return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] ?: @""; }
static NSString *pipe_row(NSString *vs) {   // an R row: the 544-byte key in hex, its vertex shader named first (nvn_pso_skey.h)
    NSMutableString *hex = [NSMutableString stringWithString:@"R\t"];
    for (NSUInteger i = 0; i < 544; i++) [hex appendFormat:@"%02x", i < vs.length ? [vs characterAtIndex:i] : 0];
    return hex;
}

/* A seed with one shader's variant (its texture slot 0 any kind but depth) and warm lists holding R ab, B ab, T ef. */
static int prune_probe(NSString *path) {
    NSString *dir = path.stringByDeletingLastPathComponent, *seed = [dir stringByAppendingPathComponent:@"seed"],
             *lists = [dir stringByAppendingPathComponent:@"lists"];
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *d in @[seed, lists]) { [fm removeItemAtPath:d error:nil]; [fm createDirectoryAtPath:d withIntermediateDirectories:YES attributes:nil error:nil]; }
    for (NSString *ext in @[@"done", @"pruned"]) [fm removeItemAtPath:totk_contrib_side(path, ext) error:nil];
    put([seed stringByAppendingPathComponent:@"seed-manifest.json"], @"{}");
    put([seed stringByAppendingPathComponent:[key(1) stringByAppendingString:@".variants"]],
        [NSString stringWithFormat:@"[{\"variant\":\"%@-0\",\"deps\":{\"tex\":{\"0\":\"*\"}}}]", key(1)]);
    put([lists stringByAppendingPathComponent:@"pso_manifest.tsv"], @"# list\nS\tx_b\t1\nR\tab\nB\tab\t-\tv\nT\tef\t1:0;\n");
    NSString *plain = [NSString stringWithFormat:@"V\t%@\t{\"attr\":{},\"fout\":{},\"img\":{},\"tex\":{\"0\":\"2d\"}}", key(1)],
             *depth = [NSString stringWithFormat:@"V\t%@\t{\"attr\":{},\"fout\":{},\"img\":{},\"tex\":{\"0\":\"*_depth\"}}", key(1)],
             *other = [NSString stringWithFormat:@"V\t%@\t{}", key(2)];
    /* A pipeline whose shader the build names otherwise: the file's portable name (totk_contrib_listed_name, its V row
       beside it) becomes the seed's variant that serves that bind state, as shader_list.py resolve names the warm lists. */
    NSDictionary *cube = @{@"tex": @{@"0": @"cube"}};
    put([seed stringByAppendingPathComponent:[key(3) stringByAppendingString:@".variants"]],
        [NSString stringWithFormat:@"[{\"variant\":\"%@-aaaaaaaaaaaa\",\"deps\":{\"tex\":{\"0\":\"cube\"}}}]", key(3)]);
    put([seed stringByAppendingPathComponent:[key(5) stringByAppendingString:@".variants"]],
        [NSString stringWithFormat:@"[{\"variant\":\"%@-bbbbbbbbbbbb\",\"deps\":{}}]", key(5)]);
    NSString *renamed = pipe_row([key(3) stringByAppendingString:@"-aaaaaaaaaaaa_b"]);
    put([lists stringByAppendingPathComponent:@"pso_manifest.tsv"], [NSString stringWithFormat:@"# list\nS\tx_b\t1\nR\tab\nB\tab\t-\tv\nT\tef\t1:0;\n%@\n", renamed]);
    NSString *cubeRow = [NSString stringWithFormat:@"V\t%@\t{\"tex\":{\"0\":\"cube\"}}", key(3)],
             *portable = pipe_row([totk_contrib_listed_name(key(3), cube) stringByAppendingString:@"_b"]),
             *unknown = pipe_row([totk_contrib_listed_name(key(5), cube) stringByAppendingString:@"_b"]);   // no V row says its state
    NSArray *served = @[plain, @"R\tab", @"B\tab\t-\tv", @"T\tef\t1:0;", cubeRow, portable],
            *kept = @[depth, other, @"R\tcd", @"T\tcd\t1:0;", unknown];
    put(path, [NSString stringWithFormat:@"# header\n# Game: TOTK 1.4.3\n%@\n%@\n%@\n%@\n%@\n%@\n%@\n%@\n%@\n%@\n%@\n",
               plain, depth, other, @"R\tab", @"R\tcd", @"B\tab\t-\tv", @"T\tef\t1:0;", @"T\tcd\t1:0;", cubeRow, portable, unknown]);
    totk_contrib_reminded(path, 8);
    check(totk_contrib_read(path).count == 11, "T rows are rows too");
    check(totk_contrib_prune(path, seed, lists) == 6, "six rows the app serves, one through the seed's name for its shader");
    check([totk_contrib_read(path) isEqualToSet:[NSSet setWithArray:kept]], "kept: a depth state no variant serves, a shader the seed lacks, unlisted pipelines");
    check([totk_contrib_read(totk_contrib_side(path, @"done")) isEqualToSet:[NSSet setWithArray:served]], "the served rows are in the done file");
    check([get(path) hasPrefix:@"# header\n# Game: TOTK 1.4.3\n"], "the header and game lines stay");
    check([get(totk_contrib_state(path)) integerValue] == 2, "the reminder counts from the rows left");
    check(totk_contrib_prune(path, seed, lists) == 0 && totk_contrib_read(path).count == 5, "the same app: not checked again");
    setenv("TOTK_SHADER_CONTRIB", path.fileSystemRepresentation, 1);
    totk_contrib_add_line(@"R\tab"); totk_contrib_add_line(@"T\tcd\t1:0;");
    check(totk_contrib_read(path).count == 5, "a done row or a T row the file has: not written again");
    check(totk_contrib_retire(path, [NSSet setWithObject:@"R\tcd"]) == 1, "a sent row moves");
    totk_contrib_add_line(@"R\tcd");
    check(totk_contrib_read(path).count == 4 && [totk_contrib_read(totk_contrib_side(path, @"done")) containsObject:@"R\tcd"], "a sent row: done, not written again");
    totk_contrib_never(path);
    put([lists stringByAppendingPathComponent:@"pso_title.tsv"], @"T\tcd\t1:0;\n");   // a new app: checked again
    check(totk_contrib_prune(path, seed, lists) == 1 && totk_contrib_read(path).count == 3, "a new app's lists prune again");
    check([get(totk_contrib_state(path)) hasPrefix:@"never"], "don't remind me again stays");
    check(totk_contrib_prune(path, [dir stringByAppendingPathComponent:@"none"], lists) == 0 && totk_contrib_read(path).count == 3, "no seed: nothing judged");
    printf("{\"status\":\"%s\",\"probe\":\"shader-contrib-prune\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2 && !(argc == 3 && (!strcmp(argv[2], "noseed") || !strcmp(argv[2], "game") || !strcmp(argv[2], "prune")))) return 2;
        NSString *path = @(argv[1]);
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        [NSFileManager.defaultManager removeItemAtPath:totk_contrib_state(path) error:nil];
        if (argc == 3 && !strcmp(argv[2], "prune")) return prune_probe(path);
        setenv("TOTK_SHADER_CONTRIB", argv[1], 1);
        if (argc == 3 && !strcmp(argv[2], "game")) {
            NSString *old = [NSString stringWithFormat:@"# header\nV\t%@\t{}\n", key(1)];
            [old writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
            setenv("TOTK_CONTRIB_GAME", "TOTK 1.4.3", 1);
            totk_contrib_add(key(1), @{});                     // a row it has: nothing written, no tag yet
            check([[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] isEqualToString:old], "a known row: untouched");
            totk_contrib_add(key(2), @{}); totk_contrib_add(key(3), @{});
            NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            NSString *want = [NSString stringWithFormat:@"%@# Game: TOTK 1.4.3\nV\t%@\t{}\nV\t%@\t{}\n", old, key(2), key(3)];
            check([text isEqualToString:want], "the game line once, before the first new row");
            check(totk_contrib_read(path).count == 3, "the game line is no row");
            printf("{\"status\":\"%s\",\"probe\":\"shader-contrib-game\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
            return failures != 0;
        }
        if (argc == 3) {
            setenv("TOTK_SHADER_SEED", "off", 1);
            totk_contrib_add(key(1), @{@"tex": @{@"9": @"2d"}});
            check(![NSFileManager.defaultManager fileExistsAtPath:path] && !totk_contrib_path(), "without the seed: no file");
            printf("{\"status\":\"%s\",\"probe\":\"shader-contrib-noseed\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
            return failures != 0;
        }
        NSDictionary *deps = @{@"tex": @{@"9": @"2d", @"10": @"3d"}, @"attr": @{@"0": @"float"}, @"fout": @{}, @"img": @{@"1": @{@"type": @"2d", @"fmt": @"r32f"}}, @"local": @[@1]};
        totk_contrib_add(key(1), deps);
        totk_contrib_add(key(1), deps);                    // the same shader and state: once
        totk_contrib_add(key(1), @{@"tex": @{@"9": @"2darray"}});
        NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        NSArray *lines = [text componentsSeparatedByString:@"\n"];
        check(lines.count == 6 && [lines[0] hasPrefix:@"# "] && [lines[2] containsString:@TOTK_CONTRIB_ISSUE_URL], "three header lines, then the rows");
        check([lines[3] isEqualToString:[NSString stringWithFormat:@"V\t%@\t{\"attr\":{\"0\":\"float\"},\"fout\":{},\"img\":{\"1\":{\"fmt\":\"r32f\",\"type\":\"2d\"}},\"tex\":{\"10\":\"3d\",\"9\":\"2d\"}}", key(1)]],
              "sorted compact deps, only tex / img / attr / fout");
        check(totk_contrib_read(path).count == 2 && totk_contrib_due(path) == 0, "two rows, no reminder yet");
        enum { S = TOTK_CONTRIB_STEP };
        for (unsigned i = 2; i < S - 1; i++) totk_contrib_add(key(i), deps);   // rows 3..S-1
        check(totk_contrib_read(path).count == S - 1 && totk_contrib_due(path) == 0, "S-1 rows: not yet");
        totk_contrib_add(key(S - 1), deps);
        check(totk_contrib_due(path) == S, "S rows: due");
        totk_contrib_reminded(path, S);
        check(totk_contrib_due(path) == 0, "reminded: not again at once");
        for (unsigned i = S; i < 2 * S - 1; i++) totk_contrib_add(key(i), deps);   // rows S+1..2S-1
        check(totk_contrib_due(path) == 0, "2S-1 rows: the next step is not reached");
        totk_contrib_add(key(2 * S - 1), deps);
        check(totk_contrib_due(path) == 2 * S, "2S rows: due again");
        totk_contrib_never(path);
        for (unsigned i = 2 * S; i < 2 * S + 60; i++) totk_contrib_add(key(i), deps);
        check(totk_contrib_due(path) == 0 && totk_contrib_read(path).count == 2 * S + 60, "never: still noted, never due");
        NSString *pipe = [@"R\t" stringByAppendingString:[@"" stringByPaddingToLength:64 withString:@"ab" startingAtIndex:0]];
        totk_contrib_add_line(pipe); totk_contrib_add_line(pipe);                      // a render pipeline: once
        totk_contrib_add_line([[pipe stringByReplacingOccurrencesOfString:@"R\t" withString:@"B\t"]
                               stringByAppendingString:@"\t-\t3:7:0x1p-1,0x1p-1,0x1p-1,0x1p-1;"]);   // its border recipe
        check(totk_contrib_read(path).count == 2 * S + 62, "pipeline rows count with the shaders");
        unsetenv("TOTK_SHADER_CONTRIB");
        check(totk_contrib_due(nil) == 0, "no file: nothing due");
        NSString *listed = totk_contrib_listed_name(key(7), @{@"tex": @{@"9": @"*", @"10": @"*_depth", @"3": @"cube"}, @"attr": @{@"6": @"uint"}, @"fout": @{@"0": @"float"}});
        printf("{\"status\":\"%s\",\"probe\":\"shader-contrib\",\"checked\":%d,\"failures\":%d,\"listed\":\"%s\"}\n", failures ? "fail" : "pass", checked, failures, listed.UTF8String);
        return failures != 0;
    }
}
