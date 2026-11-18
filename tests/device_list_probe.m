// tests/device_list_probe.m -- runs totk_device_list_merge (games/totk/third_party/d2r_gfx/gfx/nvn/totk_device_list.h)
// on files in a temporary folder; exits 1 with the first failed check. Driven by tests/test_device_list.py.
#import <Foundation/Foundation.h>
#include "totk_device_list.h"

static int failed;
static void expect(BOOL ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failed = 1; } }
static NSString *read_text(NSString *p) { return [NSString stringWithContentsOfFile:p encoding:NSUTF8StringEncoding error:nil]; }
static void write_text(NSString *p, NSString *t) { [t writeToFile:p atomically:YES encoding:NSUTF8StringEncoding error:nil]; }

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return 2;
        NSString *dir = @(argv[1]);
        NSString *old = [dir stringByAppendingPathComponent:@"pso_user.tsv"], *list = [dir stringByAppendingPathComponent:@"pso_collected.tsv"];
        NSString *app = [dir stringByAppendingPathComponent:@"pso_manifest.tsv"];
        // the list's last row was cut short of its newline; the old list repeats a row, has a comment and an app row
        write_text(list, @"R\tA\nR\tB");
        write_text(old, @"# header\nR\tB\nR\tC\nS\tD\nR\tC\nR\tAPP\nT\tE\t1:2;\n");
        write_text(app, @"R\tAPP\n");
        long n = totk_device_list_merge(old, list, @[app]);
        expect(n == 3, "appends the rows the list and the app's lists lack, once each");
        expect([read_text(list) isEqualToString:@"R\tA\nR\tB\nR\tC\nS\tD\nT\tE\t1:2;\n"], "appends in order after a newline");
        expect(![NSFileManager.defaultManager fileExistsAtPath:old], "the old list is moved away");
        expect([read_text([old stringByAppendingString:@".merged"]) hasPrefix:@"# header"], "the old list is kept as .merged");
        expect(totk_device_list_merge(old, list, @[app]) == 0, "a second start merges nothing");
        // a later old list (an older app wrote one again): merged again, the first .merged kept
        write_text(old, @"R\tC\nR\tF\n");
        expect(totk_device_list_merge(old, list, @[app]) == 1, "merges a later old list's new rows");
        expect([NSFileManager.defaultManager fileExistsAtPath:[old stringByAppendingString:@".merged-2"]], "keeps every old list");
        expect([read_text(list) hasSuffix:@"T\tE\t1:2;\nR\tF\n"], "no row twice");
        // no list yet (a new cache folder): it is made
        NSString *fresh = [dir stringByAppendingPathComponent:@"new/pso_collected.tsv"];
        [NSFileManager.defaultManager createDirectoryAtPath:fresh.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        write_text(old, @"R\tG\n");
        expect(totk_device_list_merge(old, fresh, nil) == 1 && [read_text(fresh) isEqualToString:@"R\tG\n"], "makes the list when it is missing");
        // a list that cannot be written: the old list stays for the next start
        write_text(old, @"R\tH\n");
        expect(totk_device_list_merge(old, [dir stringByAppendingPathComponent:@"missing/folder/pso_collected.tsv"], nil) == -1, "reports a list it cannot write");
        expect([NSFileManager.defaultManager fileExistsAtPath:old], "keeps the old list when the merge failed");
        expect(totk_device_list_merge(nil, list, nil) == 0, "no old list: nothing to do");
    }
    return failed;
}
