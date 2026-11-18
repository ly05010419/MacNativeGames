// What a new device cache folder takes over from the last one (2026-11-16, user: "12 做").
// Pure filesystem (Foundation only), compiled straight out of this file by tests/test_rt_cache_carry.py.
//
// The device's cache folder is named after the translation chain and context (rt-<chain>[-<context>], the launchers and
// totk_rt_dir), so every new chain or context began with an empty one: this device's pipeline archive and its collected
// list were left behind, and its first start compiled again every pipeline it had met in play. The translations stay per
// folder (another translator may translate otherwise), but the pipeline archive is good across chains, as the archive's
// identity is (totk_pipeline_archive.h par_same_identity, 8c09c8e3): Metal takes a pipeline from a file only with the very
// functions it was made from, and par_stale compiles a changed one again. The collected list's rows are warmed only when
// their variants exist, and a superseded variant is put on the seed's (totk_variant_rows.h).
#pragma once
#import <Foundation/Foundation.h>
#include <stdio.h>

/* The other rt-* folders beside `dir` (earlier chains and contexts, and folders another translator wrote, set aside as
   <dir>-retired-...), the most recently changed first: where the variants a carried collected list names still have their
   indexes (.variants, with the deps a row is put on the app's variants by; totk_shader_rt.m totk_rt_row_prefer_seed).
   2026-11-16: the folder of 09:32 was set aside for another translator, the list came along and its 100 mesh rows named the
   task variant …-04ec2b7c22af, whose deps (the same as the seed's …-b7530f698e76) were only in the set-aside folder: the
   rows stayed as they were, built nothing a draw takes, and those pipelines were compiled at the draw (226 ms, 855 ms). */
__attribute__((unused)) static NSArray<NSString *> *totk_rt_siblings(NSString *dir) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *own = dir.lastPathComponent, *parent = dir.stringByDeletingLastPathComponent;
    NSMutableArray<NSArray *> *found = [NSMutableArray array];
    if (![own hasPrefix:@"rt-"]) return @[];
    for (NSString *name in [fm contentsOfDirectoryAtPath:parent.length ? parent : @"." error:nil] ?: @[]) {
        if (![name hasPrefix:@"rt-"] || [name isEqualToString:own]) continue;
        NSString *path = [parent stringByAppendingPathComponent:name];
        BOOL isDir = NO; if (![fm fileExistsAtPath:path isDirectory:&isDir] || !isDir) continue;
        NSDate *when = [fm attributesOfItemAtPath:path error:nil].fileModificationDate ?: NSDate.distantPast;
        [found addObject:@[when, path]];
    }
    [found sortUsingComparator:^NSComparisonResult(NSArray *a, NSArray *b) { return [b[0] compare:a[0]]; }];
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSArray *f in found) [out addObject:f[1]];
    return out;
}

/* For `dir` (a folder named rt-...): each of pipelines/ and pso_collected.tsv it lacks is moved (renamed, the same volume)
   from the sibling rt-* folder whose copy changed last. Returns the paths moved from; nothing for another folder name. */
static NSArray<NSString *> *totk_rt_carry_over(NSString *dir) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSMutableArray<NSString *> *moved = [NSMutableArray array];
    NSString *own = dir.lastPathComponent, *parent = dir.stringByDeletingLastPathComponent;
    if (![own hasPrefix:@"rt-"]) return moved;
    for (NSString *item in @[@"pipelines", @"pso_collected.tsv"]) {
        NSString *target = [dir stringByAppendingPathComponent:item];
        if ([fm fileExistsAtPath:target]) continue;
        NSString *best = nil; NSDate *latest = nil;
        for (NSString *name in [fm contentsOfDirectoryAtPath:parent.length ? parent : @"." error:nil] ?: @[]) {
            if (![name hasPrefix:@"rt-"] || [name isEqualToString:own]) continue;
            NSString *from = [[parent stringByAppendingPathComponent:name] stringByAppendingPathComponent:item];
            NSDate *when = [fm attributesOfItemAtPath:from error:nil].fileModificationDate;
            if (when && (!latest || [when compare:latest] == NSOrderedDescending)) { best = from; latest = when; }
        }
        if (best && rename(best.fileSystemRepresentation, target.fileSystemRepresentation) == 0) [moved addObject:best];
    }
    return moved;
}
