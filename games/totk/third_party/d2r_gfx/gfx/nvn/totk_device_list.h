// totk_device_list.h -- this device's one warm-up list: pso_collected.tsv in the shader cache folder.
//
// Until 2026-11-16 pso_user_record wrote every new row twice: to the profile's pso_user.tsv and to the cache folder's
// pso_collected.tsv (on the Mac's Ocarina of Time 5,023 and 2,930 rows, 2,830 in both, 652 of pso_user.tsv's already in
// the app's list). The user chose one list ("1 做"): pso_collected.tsv, which moves with the cache folder when the
// translation changes (totk_rt_carry.h) and lives in the app's Documents on iOS (totk_shader_rt.m). It is what rebuilds
// this device's pipeline archive after a system update makes the archive invalid; the app's lists come from the inbox.
// 本机只留一份预热清单：缓存目录里的 pso_collected.tsv；旧的 pso_user.tsv 合并进来一次后改名留着。
#pragma once
#import <Foundation/Foundation.h>

/* Merges `old` (a profile's pso_user.tsv) into `list` (pso_collected.tsv) once: the rows of `old` that neither `list`
   nor any file of `known` (the app's lists) holds are appended to `list` in their order, then `old` is renamed
   `<old>.merged` (kept: a player's data is not deleted). Returns the rows appended, 0 when there is no `old`, and -1 when
   `old` cannot be read or `list` cannot be written (then `old` stays, and the next start tries again: a row already
   appended is not appended twice). */
static long totk_device_list_merge(NSString *old, NSString *list, NSArray<NSString *> *known) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (!old.length || !list.length || ![fm fileExistsAtPath:old]) return 0;
    long added = 0;
    @autoreleasepool {
        NSString *text = [NSString stringWithContentsOfFile:old encoding:NSUTF8StringEncoding error:nil];
        if (!text) return -1;
        NSData *before = [NSData dataWithContentsOfFile:list];
        NSMutableSet<NSString *> *have = [NSMutableSet set];
        for (NSString *path in [@[list] arrayByAddingObjectsFromArray:known ?: @[]]) @autoreleasepool {
            NSString *t = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *line in [t componentsSeparatedByString:@"\n"]) if (line.length) [have addObject:line];
        }
        NSMutableString *add = [NSMutableString string];
        // a list whose last row has no newline yet (a run that stopped mid-write) gets one first
        if (before.length && ((const char *)before.bytes)[before.length - 1] != '\n') [add appendString:@"\n"];
        for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
            if (!line.length || [line hasPrefix:@"#"] || [have containsObject:line]) continue;
            [have addObject:line]; [add appendString:line]; [add appendString:@"\n"]; added++;
        }
        if (added) {
            if (!before && ![fm createFileAtPath:list contents:nil attributes:nil]) return -1;
            NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:list];
            if (!h) return -1;
            NSError *e = nil;
            BOOL ok = [h seekToEndReturningOffset:nil error:&e] && [h writeData:[add dataUsingEncoding:NSUTF8StringEncoding] error:&e];
            [h closeAndReturnError:nil];
            if (!ok) return -1;
        }
        NSString *done = [old stringByAppendingString:@".merged"];
        for (int i = 2; [fm fileExistsAtPath:done]; i++) done = [NSString stringWithFormat:@"%@.merged-%d", old, i];
        if (![fm moveItemAtPath:old toPath:done error:nil]) return -1;
    }
    return added;
}
