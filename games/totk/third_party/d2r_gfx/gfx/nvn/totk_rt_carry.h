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
