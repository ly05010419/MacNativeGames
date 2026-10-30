// Import the app's game-derived startup shaders into writable user storage.
// Kept independent of the renderer so validation can run without a GPU/game.
#pragma once
#import <Foundation/Foundation.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>
#include "dmsh_chain.h"

static NSString *totk_seed_hash(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *s = [NSMutableString stringWithCapacity:64];
    for (unsigned i = 0; i < sizeof bytes; i++) [s appendFormat:@"%02x", bytes[i]];
    return s;
}

// What the cache receipt names: the translation chain, the shader count and the file table (name -> SHA-256) the cache got.
// The preparation's own fields (how long it took, the preparing script's hash) change with every packaging; a hash of the
// raw manifest made a repackaged app with the same shaders check all 24,871 files again on its first start (10-18 s, 2026-10-18).
static NSString *totk_seed_fingerprint(NSDictionary *m, NSArray<NSString *> *names) {
    NSDictionary *files = m[@"files"];
    NSMutableString *s = [NSMutableString stringWithFormat:@"chain %@\nschema %@\nshaders %@\n", m[@"chain"], m[@"schema"], m[@"shaders"]];
    for (NSString *name in names) [s appendFormat:@"%@ %@\n", name, files[name]];
    return totk_seed_hash([s dataUsingEncoding:NSUTF8StringEncoding]);
}

// Progress of a running import, pass 0 checking the source, pass 1 copying it (tens of thousands of files: minutes on a
// phone); set by the runtime (totk_shader_rt.m), nil in tests.
static void (*totk_seed_progress)(int pass, NSUInteger done, NSUInteger total);

// Odyssey (22 October 2026, after upstream 8e53dc5): the first start after packaging spent 32 s hashing and 80 s cloning
// the 129,602 seed files one by one. The hashes are now checked on every core (blocks of 4,096 files, progress between
// blocks), and the import is one clone of the whole seed folder into a sibling of the cache. The entries already in the
// cache are cloned over it (.variants merged, the cache's other files and folders winning over the seed's, except a
// different .metallib, as in the per-file path) and the two folders are swapped atomically (renamex_np RENAME_SWAP), so
// the cache is never missing or half merged. Upstream clones only into an empty cache. NO: nothing changed (another
// volume, a failed clone or merge); the caller imports file by file.
// The names in a folder. readdir, not NSFileManager: listing the 134k entries of a seeded cache took 7.7 s with
// contentsOfDirectoryAtPath and 0.9 s with readdir (22 October 2026).
static NSMutableSet<NSString *> *totk_seed_list(NSString *dir) {
    NSMutableSet<NSString *> *out = [NSMutableSet set];
    DIR *d = opendir(dir.fileSystemRepresentation); if (!d) return out;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        NSString *n = [[NSString alloc] initWithBytes:e->d_name length:e->d_namlen encoding:NSUTF8StringEncoding]; if (n) [out addObject:n];
    }
    closedir(d); return out;
}
static BOOL totk_seed_verify(NSString *source, NSArray<NSString *> *names, NSDictionary *files) {
    __block _Atomic int bad = 0; const NSUInteger block = 4096;
    for (NSUInteger start = 0; start < names.count && !atomic_load(&bad); start += block) {
        NSUInteger n = MIN(block, names.count - start);
        dispatch_apply(n, DISPATCH_APPLY_AUTO, ^(size_t i) { @autoreleasepool {
            if (atomic_load(&bad)) return;
            NSString *name = names[start + i];
            NSData *data = [NSData dataWithContentsOfFile:[source stringByAppendingPathComponent:name] options:NSDataReadingMappedIfSafe error:nil];
            if (!data || ![totk_seed_hash(data) isEqual:files[name]]) atomic_store(&bad, 1);
        } });
        if (totk_seed_progress) totk_seed_progress(0, start + n, names.count);
    }
    return !atomic_load(&bad);
}
static BOOL totk_seed_merge_variants(NSString *userPath, NSString *seedPath) {   // user rows first, then the seed's new ones
    NSData *u = [NSData dataWithContentsOfFile:userPath], *s = [NSData dataWithContentsOfFile:seedPath];
    id user = u ? [NSJSONSerialization JSONObjectWithData:u options:NSJSONReadingMutableContainers error:nil] : nil;
    id seed = s ? [NSJSONSerialization JSONObjectWithData:s options:0 error:nil] : nil;
    if (![user isKindOfClass:NSArray.class] || ![seed isKindOfClass:NSArray.class]) return NO;
    NSMutableArray *merged = user; NSMutableSet *known = [NSMutableSet set];
    for (id row in merged) if ([row isKindOfClass:NSDictionary.class] && row[@"variant"]) [known addObject:row[@"variant"]];
    for (id row in seed) {
        if (![row isKindOfClass:NSDictionary.class] || ![row[@"variant"] isKindOfClass:NSString.class] || ![row[@"deps"] isKindOfClass:NSDictionary.class]) return NO;
        if (![known containsObject:row[@"variant"]]) { [merged addObject:row]; [known addObject:row[@"variant"]]; }
    }
    NSData *out = [NSJSONSerialization dataWithJSONObject:merged options:NSJSONWritingSortedKeys error:nil];
    return out && [out writeToFile:seedPath options:NSDataWritingAtomic error:nil];
}
// `next`: the seed folder already cloned next to the cache (totk_seed_import_into starts that clone while the hashes
// are checked: 12.5 s of APFS metadata for 129,602 files, about as long as the checking).
static BOOL totk_seed_clone_merge(NSString *source, NSString *cache, NSString *next, NSSet<NSString *> *keep, NSUInteger *merged_out) {
    NSFileManager *fm = NSFileManager.defaultManager;
    chmod(next.fileSystemRepresentation, 0755);
    for (NSString *name in totk_seed_list(source))
        if (![keep containsObject:name]) [fm removeItemAtPath:[next stringByAppendingPathComponent:name] error:nil];
    NSUInteger merged = 0;
    for (NSString *name in totk_seed_list(cache)) {
        if ([name hasSuffix:@".import"] || [name hasPrefix:@"."]) continue;   // a per-file import's temporary leftover
        NSString *from = [cache stringByAppendingPathComponent:name], *to = [next stringByAppendingPathComponent:name];
        BOOL there = [fm fileExistsAtPath:to];
        if (there && [name hasSuffix:@".variants"]) {
            if (!totk_seed_merge_variants(from, to)) { [fm removeItemAtPath:next error:nil]; return NO; }
            merged++; continue;
        }
        if (there && [name hasSuffix:@".metallib"] && ![[NSData dataWithContentsOfFile:from] isEqualToData:[NSData dataWithContentsOfFile:to]]) continue;
        if (there) [fm removeItemAtPath:to error:nil];
        if (clonefile(from.fileSystemRepresentation, to.fileSystemRepresentation, CLONE_NOOWNERCOPY) && ![fm copyItemAtPath:from toPath:to error:nil]) {
            [fm removeItemAtPath:next error:nil]; return NO;
        }
        merged++;
    }
    if (renamex_np(next.fileSystemRepresentation, cache.fileSystemRepresentation, RENAME_SWAP)) { [fm removeItemAtPath:next error:nil]; return NO; }
    // `next` now holds the previous cache; everything in it was cloned over, so it goes in the background.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ [NSFileManager.defaultManager removeItemAtPath:next error:nil]; });
    if (merged_out) *merged_out = merged;
    return YES;
}
// marker: a receipt in the cache that skips re-importing the same files; nil imports every time (fill packs).
static NSDictionary *totk_seed_import_into(NSString *source, NSString *cache, NSString *marker, NSError **error) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *manifestPath = [source stringByAppendingPathComponent:@"seed-manifest.json"];
    if (![fm fileExistsAtPath:manifestPath]) return @{@"available": @NO}; // development/older bundles
    NSData *raw = [NSData dataWithContentsOfFile:manifestPath options:0 error:error];
    NSDictionary *m = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:error] : nil;
    NSString *problem = nil;
    if (![m isKindOfClass:NSDictionary.class] || ![m[@"chain"] isEqual:@DMSH_CHAIN] ||
        ![m[@"schema"] isEqual:@1] || ![m[@"shaders"] isKindOfClass:NSNumber.class] || [m[@"shaders"] unsignedLongLongValue] == 0 ||
        ![m[@"files"] isKindOfClass:NSDictionary.class] || ![m[@"files"] count]) problem = @"Startup shader seed identity is invalid";
    if (problem) goto fail;
    {
        NSDictionary *files = m[@"files"];
        NSArray<NSString *> *names = [files.allKeys sortedArrayUsingSelector:@selector(compare:)];
        NSString *fingerprint = totk_seed_fingerprint(m, names);
        NSData *oldData = marker ? [NSData dataWithContentsOfFile:marker] : nil;
        NSDictionary *old = oldData ? [NSJSONSerialization JSONObjectWithData:oldData options:0 error:nil] : nil;
        NSRegularExpression *safe = [NSRegularExpression regularExpressionWithPattern:
            @"^(?:[0-9a-f]{64}\\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\\.(?:metal|json|metallib)|pso_manifest\\.tsv)$" options:0 error:nil];
        BOOL installed = [old isKindOfClass:NSDictionary.class] && [old[@"files_sha256"] isEqual:fingerprint];   // receipts from before 2026-10-18 (manifest_sha256) are checked once more
        // One listing of the cache instead of a stat per name: 129,602 fileExistsAtPath calls in a folder of 134k entries
        // took 16.5 s on every start (Odyssey, 22 October 2026); the listing takes well under a second.
        NSSet<NSString *> *present = installed ? totk_seed_list(cache) : nil;
        for (id name in names) {
            if (![name isKindOfClass:NSString.class] || ![safe numberOfMatchesInString:name options:0 range:NSMakeRange(0, [name length])]) {
                problem = @"Unsafe startup shader seed filename"; goto fail;
            }
            if (installed && ![present containsObject:name]) installed = NO;
        }
        if (installed) return @{@"available": @YES, @"shaders": m[@"shaders"], @"imported_files": @0};
        // Validate the complete source before changing any existing cache entry. The folder clone goes into a sibling of
        // the cache at the same time; it touches nothing the game uses and is discarded if a hash does not match.
        NSString *next = [cache stringByAppendingFormat:@".import-%d", getpid()];
        __block BOOL cloned = NO;
        dispatch_group_t cloning = dispatch_group_create();
        if (marker) {
            // Folders a previous start left (the old cache is removed in the background after the swap; quitting first leaves it).
            NSString *parent = cache.stringByDeletingLastPathComponent, *prefix = [cache.lastPathComponent stringByAppendingString:@".import-"];
            for (NSString *name in totk_seed_list(parent))
                if ([name hasPrefix:prefix]) [fm removeItemAtPath:[parent stringByAppendingPathComponent:name] error:nil];
            dispatch_group_async(cloning, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                cloned = !clonefile(source.fileSystemRepresentation, next.fileSystemRepresentation, CLONE_NOOWNERCOPY);
            });
        }
        BOOL verified = totk_seed_verify(source, names, files);
        dispatch_group_wait(cloning, DISPATCH_TIME_FOREVER);
        if (!verified) {
            if (cloned) [fm removeItemAtPath:next error:nil];
            problem = @"Startup shader seed integrity check failed"; goto fail;
        }
        if (![fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:error]) return nil;
        NSUInteger kept = 0;
        if (cloned && totk_seed_clone_merge(source, cache, next, [NSSet setWithArray:names], &kept)) {
            NSData *receipt = [NSJSONSerialization dataWithJSONObject:@{@"files_sha256": fingerprint} options:0 error:error];
            if (!receipt || ![receipt writeToFile:marker options:NSDataWritingAtomic error:error]) return nil;
            if (totk_seed_progress) totk_seed_progress(1, names.count, names.count);
            return @{@"available": @YES, @"shaders": m[@"shaders"], @"imported_files": @(names.count), @"cloned_folder": @YES,
                     @"cache_entries_kept": @(kept)};
        }
        NSUInteger imported = 0, copied = 0;
        // Make shader files available before publishing their variant indexes.
        for (NSNumber *indexes in @[@NO, @YES]) for (NSString *name in names) {
            BOOL isIndex = [name hasSuffix:@".variants"];
            if (isIndex != indexes.boolValue) continue;
            if (totk_seed_progress && (++copied % 256 == 0 || copied == names.count)) totk_seed_progress(1, copied, names.count);
            NSString *target = [cache stringByAppendingPathComponent:name], *from = [source stringByAppendingPathComponent:name];
            if (!isIndex) {
                if ([fm fileExistsAtPath:target]) {
                    // Preserve shaders collected on this Mac. A device library (iOS) is only ever made from its source on
                    // the Mac, so a different one is a rebuild, e.g. with safe math instead of Metal's default fast math.
                    if (![name hasSuffix:@".metallib"]) continue;
                    NSData *data = [NSData dataWithContentsOfFile:from options:0 error:error];
                    if (!data) return nil;
                    if ([[NSData dataWithContentsOfFile:target] isEqualToData:data]) continue;
                }
                // Clone, not read and write: the app bundle and the cache share an APFS volume, so tens of thousands of
                // files cost metadata only (2026-10-19: reading and writing 44,744 took 59 s on an M4, two minutes on a
                // phone). Another volume falls back to a copy. A temporary name and a rename keep a half-written file
                // out of the cache.
                NSString *temp = [target stringByAppendingFormat:@".%d.import", getpid()];
                unlink(temp.fileSystemRepresentation);
                if (clonefile(from.fileSystemRepresentation, temp.fileSystemRepresentation, 0) &&
                    ![fm copyItemAtPath:from toPath:temp error:error]) return nil;
                if (rename(temp.fileSystemRepresentation, target.fileSystemRepresentation)) {
                    if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
                    unlink(temp.fileSystemRepresentation);
                    return nil;
                }
                imported++;
                continue;
            }
            NSData *data = [NSData dataWithContentsOfFile:from options:0 error:error];
            if (!data) return nil;
            {
                id seed = [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:error];
                NSData *existing = [NSData dataWithContentsOfFile:target];
                id user = existing ? [NSJSONSerialization JSONObjectWithData:existing options:NSJSONReadingMutableContainers error:error] : nil;
                if (![seed isKindOfClass:NSArray.class] || (existing && ![user isKindOfClass:NSArray.class])) {
                    problem = @"Invalid startup shader variant index"; goto fail;
                }
                NSMutableArray *merged = user ?: [NSMutableArray array];
                NSMutableSet *known = [NSMutableSet set];
                for (id row in merged) if ([row isKindOfClass:NSDictionary.class] && row[@"variant"]) [known addObject:row[@"variant"]];
                BOOL changed = !existing;
                for (id row in seed) {
                    if (![row isKindOfClass:NSDictionary.class] || ![row[@"variant"] isKindOfClass:NSString.class] ||
                        ![row[@"deps"] isKindOfClass:NSDictionary.class]) { problem = @"Invalid prepared shader dependencies"; goto fail; }
                    if (![known containsObject:row[@"variant"]]) { [merged addObject:row]; [known addObject:row[@"variant"]]; changed = YES; }
                }
                if (!changed) continue;
                data = [NSJSONSerialization dataWithJSONObject:merged options:NSJSONWritingSortedKeys error:error];
                if (!data) return nil;
            }
            if (![data writeToFile:target options:NSDataWritingAtomic error:error]) return nil;
            imported++;
        }
        NSData *receipt = [NSJSONSerialization dataWithJSONObject:@{@"files_sha256": fingerprint} options:0 error:error];
        if (marker && (!receipt || ![receipt writeToFile:marker options:NSDataWritingAtomic error:error])) return nil;
        return @{@"available": @YES, @"shaders": m[@"shaders"], @"imported_files": @(imported)};
    }
fail:
    if (error) *error = [NSError errorWithDomain:@"MacNativeGames.ShaderSeed" code:1
        userInfo:@{NSLocalizedDescriptionKey: problem ?: @"Cannot import startup shaders"}];
    return nil;
}

static NSDictionary *totk_seed_import(NSString *source, NSString *cache, NSError **error) {
    return totk_seed_import_into(source, cache, [cache stringByAppendingPathComponent:@"seed-installed.json"], error);
}

// iOS has no translator: ios/tools/fill_shaders.py translates the variants a device missed and pushes them as packs
// (directories in the seed format) under root. Each pack is checked and merged like the seed, then deleted; a rejected
// one (an interrupted push) is deleted too, and its variants stay requested until the tool runs again.
static NSDictionary *totk_fill_import(NSString *root, NSString *cache) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSArray<NSString *> *packs = [[fm contentsOfDirectoryAtPath:root error:nil] ?: @[] sortedArrayUsingSelector:@selector(compare:)];
    NSUInteger merged = 0, imported = 0; NSMutableArray<NSString *> *rejected = [NSMutableArray array];
    for (NSString *name in packs) {
        NSString *pack = [root stringByAppendingPathComponent:name]; BOOL dir = NO;
        if (![fm fileExistsAtPath:pack isDirectory:&dir] || !dir) continue;
        NSError *error = nil;
        NSDictionary *r = totk_seed_import_into(pack, cache, nil, &error);
        if (r && [r[@"available"] boolValue]) { merged++; imported += [r[@"imported_files"] unsignedIntegerValue]; }
        else [rejected addObject:[NSString stringWithFormat:@"%@: %@", name, error ? error.localizedDescription : @"no manifest"]];
        [fm removeItemAtPath:pack error:nil];
    }
    return @{@"packs": @(merged), @"imported_files": @(imported), @"rejected": rejected};
}
