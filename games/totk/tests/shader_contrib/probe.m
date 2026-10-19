// totk_shader_contrib.h: the contribution file and its reminder. Usage: probe <file>; writes the file, prints JSON.
#import <Foundation/Foundation.h>
#include "totk_shader_contrib.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static NSString *key(unsigned i) { return [NSString stringWithFormat:@"%064x", i]; }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        NSString *path = @(argv[1]);
        [NSFileManager.defaultManager removeItemAtPath:path error:nil];
        [NSFileManager.defaultManager removeItemAtPath:[path stringByAppendingString:@".reminded"] error:nil];
        setenv("TOTK_SHADER_CONTRIB", argv[1], 1);
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
        for (unsigned i = 2; i < 499; i++) totk_contrib_add(key(i), deps);   // rows 3..499
        check(totk_contrib_read(path).count == 499 && totk_contrib_due(path) == 0, "499 rows: not yet");
        totk_contrib_add(key(499), deps);
        check(totk_contrib_due(path) == 500, "500 rows: due");
        totk_contrib_reminded(path, 500);
        check(totk_contrib_due(path) == 0, "reminded: not again at once");
        for (unsigned i = 500; i < 999; i++) totk_contrib_add(key(i), deps);   // rows 501..999
        check(totk_contrib_due(path) == 0, "999 rows: the next step is not reached");
        totk_contrib_add(key(999), deps);
        check(totk_contrib_due(path) == 1000, "1000 rows: due again");
        totk_contrib_never(path);
        for (unsigned i = 1000; i < 1600; i++) totk_contrib_add(key(i), deps);
        check(totk_contrib_due(path) == 0 && totk_contrib_read(path).count == 1600, "never: still noted, never due");
        unsetenv("TOTK_SHADER_CONTRIB");
        check(totk_contrib_due(nil) == 0, "no file: nothing due");
        printf("{\"status\":\"%s\",\"probe\":\"shader-contrib\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
        return failures != 0;
    }
}
