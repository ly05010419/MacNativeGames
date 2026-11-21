// Import the app's game-derived startup shaders into writable user storage.
// Kept independent of the renderer so validation can run without a GPU/game.
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include "dmsh_chain.h"

static NSString *totk_seed_hash(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *s = [NSMutableString stringWithCapacity:64];
    for (unsigned i = 0; i < sizeof bytes; i++) [s appendFormat:@"%02x", bytes[i]];
    return s;
}

static NSDictionary *totk_seed_import(NSString *source, NSString *cache, NSError **error) {
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
        NSString *fingerprint = totk_seed_hash(raw);
        NSString *marker = [cache stringByAppendingPathComponent:@"seed-installed.json"];
        NSData *oldData = [NSData dataWithContentsOfFile:marker];
        NSDictionary *old = oldData ? [NSJSONSerialization JSONObjectWithData:oldData options:0 error:nil] : nil;
        NSDictionary *files = m[@"files"];
        NSArray<NSString *> *names = [files.allKeys sortedArrayUsingSelector:@selector(compare:)];
        NSRegularExpression *safe = [NSRegularExpression regularExpressionWithPattern:
            @"^(?:[0-9a-f]{64}\\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\\.(?:metal|json)|pso_manifest\\.tsv)$" options:0 error:nil];
        BOOL installed = [old isKindOfClass:NSDictionary.class] && [old[@"manifest_sha256"] isEqual:fingerprint];
        for (id name in names) {
            if (![name isKindOfClass:NSString.class] || ![safe numberOfMatchesInString:name options:0 range:NSMakeRange(0, [name length])]) {
                problem = @"Unsafe startup shader seed filename"; goto fail;
            }
            if (![fm fileExistsAtPath:[cache stringByAppendingPathComponent:name]]) installed = NO;
        }
        if (installed) return @{@"available": @YES, @"shaders": m[@"shaders"], @"imported_files": @0};
        // Validate the complete source before changing any existing cache entry.
        for (NSString *name in names) {
            NSData *data = [NSData dataWithContentsOfFile:[source stringByAppendingPathComponent:name] options:0 error:error];
            if (!data || ![totk_seed_hash(data) isEqual:files[name]]) { problem = @"Startup shader seed integrity check failed"; goto fail; }
        }
        if (![fm createDirectoryAtPath:cache withIntermediateDirectories:YES attributes:nil error:error]) return nil;
        NSUInteger imported = 0;
        // Make shader files available before publishing their variant indexes.
        for (NSNumber *indexes in @[@NO, @YES]) for (NSString *name in names) {
            BOOL isIndex = [name hasSuffix:@".variants"];
            if (isIndex != indexes.boolValue) continue;
            NSString *target = [cache stringByAppendingPathComponent:name];
            NSData *data = [NSData dataWithContentsOfFile:[source stringByAppendingPathComponent:name] options:0 error:error];
            if (!data) return nil;
            if (isIndex) {
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
            } else if ([fm fileExistsAtPath:target]) continue; // preserve shaders collected on this Mac
            if (![data writeToFile:target options:NSDataWritingAtomic error:error]) return nil;
            imported++;
        }
        NSData *receipt = [NSJSONSerialization dataWithJSONObject:@{@"manifest_sha256": fingerprint} options:0 error:error];
        if (!receipt || ![receipt writeToFile:marker options:NSDataWritingAtomic error:error]) return nil;
        return @{@"available": @YES, @"shaders": m[@"shaders"], @"imported_files": @(imported)};
    }
fail:
    if (error) *error = [NSError errorWithDomain:@"MacNativeGames.ShaderSeed" code:1
        userInfo:@{NSLocalizedDescriptionKey: problem ?: @"Cannot import startup shaders"}];
    return nil;
}
