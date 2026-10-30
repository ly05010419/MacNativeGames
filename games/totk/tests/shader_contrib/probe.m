// totk_shader_contrib.h: the contribution file and its reminder. Usage: probe <file> [noseed | game]; writes the file, prints JSON.
// game: a file an older app wrote (no "# Game:" line) gets the TOTK_CONTRIB_GAME line once, before its next row.
// noseed: with TOTK_SHADER_SEED=off nothing is written (a process of its own: totk_contrib_add keeps its file once opened).
#import <Foundation/Foundation.h>
#include "totk_shader_contrib.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static NSString *key(unsigned i) { return [NSString stringWithFormat:@"%064x", i]; }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2 && !(argc == 3 && (!strcmp(argv[2], "noseed") || !strcmp(argv[2], "game")))) return 2;
        NSString *path = @(argv[1]);
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        [NSFileManager.defaultManager removeItemAtPath:totk_contrib_state(path) error:nil];
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
