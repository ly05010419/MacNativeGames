// tests/test_archive_prune.py: par_prune_superseded (totk_pipeline_archive_files.h) on a folder of key lists, with the
// archive's globals and file names as totk_pipeline_archive.h has them. Prints "FAIL ..." lines and "done <failures>".
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <stdint.h>

#define MLOG(...) ((void)0)
static pthread_mutex_t g_par_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet<NSString *> *g_par_have, *g_par_pending, *g_par_used;
static NSMutableDictionary<NSString *, NSString *> *g_par_where;
static NSMutableDictionary *g_par_open;
static NSString *g_dir;
static NSString *par_dir(void) { return g_dir; }
static NSString *par_file(NSString *name) { return [par_dir() stringByAppendingFormat:@"/%@.metalarchive", name]; }
static NSString *par_keys_file(NSString *name) { return [par_dir() stringByAppendingFormat:@"/%@.keys", name]; }
static NSString *par_lock_file(void) { return [g_dir stringByAppendingPathComponent:@"pipelines.lock"]; }
static NSArray<NSString *> *par_pub_scan(NSString *dir, NSString *lock, BOOL clean, NSSet<NSString *> *keep) {
    (void)lock; (void)clean; (void)keep; NSMutableArray *names = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) if ([n hasSuffix:@".keys"]) [names addObject:n.stringByDeletingPathExtension];
    return [names sortedArrayUsingSelector:@selector(compare:)];
}
#include "totk_pipeline_archive_files.h"

static int failures;
static void file(NSString *name, NSArray<NSString *> *keys) {
    [@"x" writeToFile:par_file(name) atomically:NO encoding:NSUTF8StringEncoding error:nil];
    [[keys componentsJoinedByString:@"\n"] writeToFile:par_keys_file(name) atomically:NO encoding:NSUTF8StringEncoding error:nil];
}
static void expect(NSString *name, BOOL present, const char *why) {
    BOOL there = [NSFileManager.defaultManager fileExistsAtPath:par_file(name)] && [NSFileManager.defaultManager fileExistsAtPath:par_keys_file(name)];
    if (there != present) { failures++; printf("FAIL %s: %s %s\n", why, name.UTF8String, there ? "kept" : "removed"); }
}
static void fresh(NSString *root, NSString *sub) {
    g_dir = [root stringByAppendingPathComponent:sub];
    [NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil];
}

int main(void) {
    @autoreleasepool {
        NSString *root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        // Without a record of the keys used (a start, not packaging): only the files whose keys all moved go.
        fresh(root, @"no-record");
        file(@"a", @[@"k1", @"k2"]); file(@"b", @[@"k3"]); file(@"a-old", @[@"k4"]); file(@"z-new", @[@"k4"]);
        NSUInteger n = 0; par_load_dir(&n); g_par_used = nil;
        if (par_prune_superseded() != 1) { failures++; printf("FAIL no-record: removed count\n"); }
        expect(@"a", YES, "no-record"); expect(@"b", YES, "no-record"); expect(@"z-new", YES, "no-record"); expect(@"a-old", NO, "no-record: k4 is the newer file's");
        // Packaging (a recheck): a file holding none of the keys the run used holds pipelines the lists no longer have.
        fresh(root, @"recheck");
        file(@"a", @[@"k1", @"k2"]); file(@"b", @[@"k3"]); file(@"c", @[@"k5", @"k6"]); file(@"a-old", @[@"k4"]); file(@"z-new", @[@"k4"]);
        par_load_dir(&n); g_par_used = [NSMutableSet setWithArray:@[@"k1", @"k4", @"k6"]];
        if (par_prune_superseded() != 2) { failures++; printf("FAIL recheck: removed count\n"); }
        expect(@"a", YES, "recheck: k1 used"); expect(@"c", YES, "recheck: k6 used"); expect(@"z-new", YES, "recheck: k4 used");
        expect(@"b", NO, "recheck: k3 no longer listed"); expect(@"a-old", NO, "recheck: k4 is the newer file's");
        [NSFileManager.defaultManager removeItemAtPath:root error:nil];
        printf("done %d\n", failures);
    }
    return failures ? 1 : 0;
}
