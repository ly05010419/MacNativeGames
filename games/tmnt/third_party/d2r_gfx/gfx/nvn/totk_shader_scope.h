// The background warm-up scope (PR #84): the shader keys of this app's game version, from the seed's shader-keys.json
// checked against seed-manifest.json. No scope (an older app or invalid metadata) warms the whole cache as before.
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>

static NSString *totk_shader_scope_hash(NSData *data) {
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, bytes);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (unsigned i = 0; i < sizeof bytes; i++) [hex appendFormat:@"%02x", bytes[i]];
    return hex;
}

static BOOL totk_shader_scope_hex(NSString *value, NSUInteger count) {
    if (![value isKindOfClass:NSString.class] || value.length != count) return NO;
    for (NSUInteger i = 0; i < count; i++) {
        unichar c = [value characterAtIndex:i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return NO;
    }
    return YES;
}

// shader-keys.json is tied to the verified seed manifest rather than the writable
// cache: another game version's scope must never suppress this app's warm-up.
static NSDictionary *totk_shader_scope_metadata(NSString *seedDir, NSString *chain) {
    if (![seedDir isKindOfClass:NSString.class] || !seedDir.length ||
        ![chain isKindOfClass:NSString.class] || !chain.length) return nil;
    NSData *manifestData = [NSData dataWithContentsOfFile:[seedDir stringByAppendingPathComponent:@"seed-manifest.json"]];
    NSData *scopeData = [NSData dataWithContentsOfFile:[seedDir stringByAppendingPathComponent:@"shader-keys.json"]];
    if (!manifestData || !scopeData) return nil;
    id manifest = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil];
    id scope = [NSJSONSerialization JSONObjectWithData:scopeData options:0 error:nil];
    if (![manifest isKindOfClass:NSDictionary.class] || ![scope isKindOfClass:NSDictionary.class] ||
        ![manifest[@"schema"] isEqual:@1] || ![scope[@"schema"] isEqual:@1] ||
        ![manifest[@"chain"] isEqual:chain] || ![scope[@"chain"] isEqual:chain] ||
        ![manifest[@"version"] isKindOfClass:NSString.class] || ![manifest[@"version"] length] ||
        ![scope[@"version"] isEqual:manifest[@"version"]] ||
        !totk_shader_scope_hex(manifest[@"source_romfs_sha256"], 64) ||
        ![scope[@"source_romfs_sha256"] isEqual:manifest[@"source_romfs_sha256"]] ||
        ![manifest[@"files"] isKindOfClass:NSDictionary.class] ||
        ![manifest[@"files"][@"shader-keys.json"] isEqual:totk_shader_scope_hash(scopeData)] ||
        ![scope[@"keys"] isKindOfClass:NSArray.class] || ![scope[@"keys"] count]) return nil;
    for (id key in scope[@"keys"]) {
        if (!totk_shader_scope_hex(key, 64)) return nil;
    }
    return scope;
}

static BOOL totk_shader_scope_contains(NSSet<NSString *> *scope, NSString *key) {
    if (!scope) return YES;
    if (totk_shader_scope_hex(key, 64)) return [scope containsObject:key];
    if (![key isKindOfClass:NSString.class] || key.length != 79 ||
        [key characterAtIndex:64] != '-' || ![key hasSuffix:@"_b"] ||
        !totk_shader_scope_hex([key substringToIndex:64], 64) ||
        !totk_shader_scope_hex([key substringWithRange:NSMakeRange(65, 12)], 12)) return NO;
    return [scope containsObject:[key substringToIndex:64]];
}
