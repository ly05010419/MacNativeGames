// CPU-only probe of the shader seed importer (third_party/d2r_gfx/gfx/nvn/totk_shader_seed.h): folder clone into an
// empty cache, clone + merge into a cache that already holds files, receipt on the second run, and a tampered seed
// rejected with the cache untouched. Synthetic files only; no game data.
// build: xcrun clang -fobjc-arc -O1 -Wall -Wextra -Werror -Ithird_party/d2r_gfx/gfx/nvn tests/shader_seed_import/probe.m -framework Foundation -o <out>
// run:   <out> <empty scratch dir>
#import <Foundation/Foundation.h>
#include "totk_shader_seed.h"

static int failures;
#define CHECK(c, what) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s\n", what); } else printf("ok   %s\n", what); } while (0)

static NSString *K(char c) { return [@"" stringByPaddingToLength:64 withString:[NSString stringWithFormat:@"%c", c] startingAtIndex:0]; }
static NSString *V(char c, char d) { return [NSString stringWithFormat:@"%@-%@", K(c), [@"" stringByPaddingToLength:12 withString:[NSString stringWithFormat:@"%c", d] startingAtIndex:0]]; }
static void put(NSString *dir, NSString *name, NSString *text) {
    [[text dataUsingEncoding:NSUTF8StringEncoding] writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES];
}
static NSString *rows(NSArray *variants) {
    NSMutableArray *a = [NSMutableArray array];
    for (NSString *v in variants) [a addObject:@{@"variant": v, @"deps": @{}}];
    return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:a options:NSJSONWritingSortedKeys error:nil] encoding:NSUTF8StringEncoding];
}
static void make_seed(NSString *seed) {
    NSFileManager *fm = NSFileManager.defaultManager; [fm removeItemAtPath:seed error:nil];
    [fm createDirectoryAtPath:seed withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *a = V('a', '1'), *b = V('b', '1');
    put(seed, [a stringByAppendingString:@"_b.metal"], @"seed metal a");
    put(seed, [a stringByAppendingString:@"_b.json"], @"{\"chain\":\"x\"}");
    put(seed, [b stringByAppendingString:@"_b.metal"], @"seed metal b");
    put(seed, [b stringByAppendingString:@"_b.metallib"], @"seed lib b");
    put(seed, [K('a') stringByAppendingString:@".variants"], rows(@[a]));
    put(seed, [K('b') stringByAppendingString:@".variants"], rows(@[b]));
    put(seed, @"pso_manifest.tsv", @"S\tx\t0\n");
    NSMutableDictionary *files = [NSMutableDictionary dictionary];
    for (NSString *n in [fm contentsOfDirectoryAtPath:seed error:nil])
        files[n] = totk_seed_hash([NSData dataWithContentsOfFile:[seed stringByAppendingPathComponent:n]]);
    NSDictionary *m = @{@"chain": @DMSH_CHAIN, @"schema": @1, @"shaders": @2, @"files": files};
    [[NSJSONSerialization dataWithJSONObject:m options:0 error:nil] writeToFile:[seed stringByAppendingPathComponent:@"seed-manifest.json"] atomically:YES];
    put(seed, @"shader-keys.json", @"{}");   // not named by the manifest: must not reach the cache
}
static NSArray *listing(NSString *dir) {
    return [[NSFileManager.defaultManager subpathsOfDirectoryAtPath:dir error:nil] sortedArrayUsingSelector:@selector(compare:)];
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: probe <scratch dir>\n"); return 2; }
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *root = @(argv[1]), *seed = [root stringByAppendingPathComponent:@"seed"];
    make_seed(seed);
    NSString *a = V('a', '1'), *b = V('b', '1'), *u = V('a', '2'), *c = V('c', '1');

    // 1. Empty cache: one folder clone; manifest and unnamed files stay out.
    NSString *c1 = [root stringByAppendingPathComponent:@"cache1"];
    NSError *e = nil; NSDictionary *r = totk_seed_import(seed, c1, &e);
    CHECK(r && [r[@"cloned_folder"] boolValue], "empty cache: imported as one folder clone");
    CHECK([fm fileExistsAtPath:[c1 stringByAppendingPathComponent:[a stringByAppendingString:@"_b.metal"]]], "empty cache: seed shader present");
    CHECK(![fm fileExistsAtPath:[c1 stringByAppendingPathComponent:@"seed-manifest.json"]] &&
          ![fm fileExistsAtPath:[c1 stringByAppendingPathComponent:@"shader-keys.json"]], "empty cache: manifest and unnamed files not copied");
    CHECK([fm fileExistsAtPath:[c1 stringByAppendingPathComponent:@"seed-installed.json"]], "empty cache: receipt written");
    r = totk_seed_import(seed, c1, &e);
    CHECK(r && [r[@"imported_files"] unsignedIntegerValue] == 0, "second run: nothing imported");

    // 2. Cache that already holds files (a player's): their files, folders and variant rows are kept.
    NSString *c2 = [root stringByAppendingPathComponent:@"cache2"];
    [fm createDirectoryAtPath:[c2 stringByAppendingPathComponent:@"pipelines"] withIntermediateDirectories:YES attributes:nil error:nil];
    put([c2 stringByAppendingPathComponent:@"pipelines"], @"p.metalarchive", @"archive");
    put(c2, @"pso_collected.tsv", @"R\tuser\n");
    put(c2, [K('a') stringByAppendingString:@".variants"], rows(@[u]));
    put(c2, [u stringByAppendingString:@"_b.metal"], @"user metal");
    put(c2, [a stringByAppendingString:@"_b.metal"], @"user's own copy of a");
    put(c2, [b stringByAppendingString:@"_b.metallib"], @"older lib b");
    put(c2, [K('c') stringByAppendingString:@".variants"], rows(@[c]));
    r = totk_seed_import(seed, c2, &e);
    CHECK(r && [r[@"cloned_folder"] boolValue], "player cache: imported by clone + merge");
    NSArray *va = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[c2 stringByAppendingPathComponent:[K('a') stringByAppendingString:@".variants"]]] options:0 error:nil];
    CHECK(va.count == 2 && [va[0][@"variant"] isEqual:u] && [va[1][@"variant"] isEqual:a], "player cache: .variants merged, player's row first");
    CHECK([fm fileExistsAtPath:[c2 stringByAppendingPathComponent:@"pipelines/p.metalarchive"]], "player cache: pipeline archive folder kept");
    CHECK([[NSString stringWithContentsOfFile:[c2 stringByAppendingPathComponent:@"pso_collected.tsv"] encoding:NSUTF8StringEncoding error:nil] isEqual:@"R\tuser\n"], "player cache: collected list kept");
    CHECK([fm fileExistsAtPath:[c2 stringByAppendingPathComponent:[K('c') stringByAppendingString:@".variants"]]] &&
          [fm fileExistsAtPath:[c2 stringByAppendingPathComponent:[u stringByAppendingString:@"_b.metal"]]], "player cache: player-only shaders kept");
    CHECK([[NSString stringWithContentsOfFile:[c2 stringByAppendingPathComponent:[a stringByAppendingString:@"_b.metal"]] encoding:NSUTF8StringEncoding error:nil] isEqual:@"user's own copy of a"], "player cache: player's existing shader file wins");
    CHECK([[NSString stringWithContentsOfFile:[c2 stringByAppendingPathComponent:[b stringByAppendingString:@"_b.metallib"]] encoding:NSUTF8StringEncoding error:nil] isEqual:@"seed lib b"], "player cache: a different .metallib is replaced by the seed's");
    CHECK([fm fileExistsAtPath:[c2 stringByAppendingPathComponent:[b stringByAppendingString:@"_b.metal"]]], "player cache: new seed shaders added");
    NSArray *leftovers = [[fm contentsOfDirectoryAtPath:root error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF CONTAINS '.import-'"]];
    [NSThread sleepForTimeInterval:0.5];   // the old folder is removed in the background
    leftovers = [[fm contentsOfDirectoryAtPath:root error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF CONTAINS '.import-'"]];
    CHECK(leftovers.count == 0, "player cache: previous folder removed after the swap");

    // 3. Tampered seed: rejected before anything in the cache changes.
    NSString *c3 = [root stringByAppendingPathComponent:@"cache3"];
    [fm createDirectoryAtPath:c3 withIntermediateDirectories:YES attributes:nil error:nil];
    put(c3, @"pso_collected.tsv", @"R\tuser\n");
    NSArray *before = listing(c3);
    put(seed, [b stringByAppendingString:@"_b.metal"], @"tampered");
    r = totk_seed_import(seed, c3, &e);
    CHECK(!r && [e.localizedDescription containsString:@"integrity"], "tampered seed: rejected");
    CHECK([listing(c3) isEqual:before], "tampered seed: cache untouched");
    printf(failures ? "FAILED %d\n" : "all passed\n", failures);
    return failures != 0;
} }
