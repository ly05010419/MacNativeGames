// Which translator wrote a device cache folder's translations (2026-11-16, user: "修 1234").
// Pure filesystem (Foundation, CommonCrypto), compiled straight out of this file by tests/test_rt_cache_translator.py.
//
// The device's cache folder is named after the translation chain and the seed's translation context (rt-<chain>-<context>,
// the launchers and totk_rt_dir), which names the translator the seed was made with. The translations in it are made by
// another program: the App's build/shader-service/totk_xlate. Ocarina of Time's express updates from 2026-11-15 shipped
// the seed of translator 9845718d with the in-game translator 3b26a94d of 11-11 (games/oot/build.py install_translator),
// so its folder rt-...-e37674ae held 1,040 variants of the old translator under the new one's name, and a fixed App would
// have used them. The folder now records the translator that writes into it (translator.sha256): a folder another
// translator wrote is set aside whole (renamed beside it, so totk_rt_carry_over still takes its pipeline archive and its
// collected list, which do not depend on the translator) and a new one begins.
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* SHA-256 of a file as 64 lowercase hex digits; nil when it cannot be read. */
__attribute__((unused)) static NSString *totk_file_sha256(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
    if (!data) return nil;
    uint8_t md[32]; CC_SHA256(data.bytes, (CC_LONG)data.length, md);
    NSMutableString *hex = [NSMutableString stringWithCapacity:64];
    for (int i = 0; i < 32; i++) [hex appendFormat:@"%02x", md[i]];
    return hex;
}

/* The folder holds translations (a translated shader's MSL, SPIR-V or its variant index). */
__attribute__((unused)) static BOOL totk_rt_has_translations(NSString *dir) {
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] ?: @[])
        if ([name hasSuffix:@"_b.metal"] || [name hasSuffix:@".spv"] || [name hasSuffix:@".variants"]) return YES;
    return NO;
}

/* For a device cache folder `dir` (named rt-...) that `translator` (the SHA-256 of the program that translates into it)
   is about to use. Its record translator.sha256 names another translator, or it has none and holds translations while
   `unrecorded_untrusted` (a game whose Apps shipped another translator than their seed's): the folder is renamed to
   <dir>-retired-<the other's first 12 digits or "unrecorded">-<seconds>, an empty one made in its place, and the renamed
   path returned (*why says which case). Otherwise nil: the folder stays. Either way it then records `translator`.
   A folder of another name (a packaging cache), or no translator known, is left alone. A rename that fails leaves the
   folder unrecorded and sets *why (the caller logs it): its translations are not claimed for this translator. */
__attribute__((unused)) static NSString *totk_rt_translator_check(NSString *dir, NSString *translator, BOOL unrecorded_untrusted, NSString **why) {
    if (why) *why = nil;
    if (translator.length != 64 || ![dir.lastPathComponent hasPrefix:@"rt-"]) return nil;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *record = [dir stringByAppendingPathComponent:@"translator.sha256"];
    NSString *had = [[NSString stringWithContentsOfFile:record encoding:NSUTF8StringEncoding error:nil]
                     stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([had isEqualToString:translator]) return nil;
    NSString *retired = nil;
    BOOL other = had.length ? YES : (unrecorded_untrusted && totk_rt_has_translations(dir));
    if (other) {
        NSString *tag = had.length >= 12 ? [had substringToIndex:12] : @"unrecorded";
        retired = [dir stringByAppendingFormat:@"-retired-%@-%ld", tag, (long)time(NULL)];
        if (rename(dir.fileSystemRepresentation, retired.fileSystemRepresentation) != 0) {
            if (why) *why = [NSString stringWithFormat:@"cannot set the folder aside (%s); its translations stay unrecorded", strerror(errno)];
            return nil;
        }
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        if (why) *why = had.length ? [NSString stringWithFormat:@"its translations are translator %@'s", tag]
                                   : @"its translations do not name their translator, and this game's Apps shipped another one";
    }
    [[translator stringByAppendingString:@"\n"] writeToFile:record atomically:YES encoding:NSUTF8StringEncoding error:nil];
    return retired;
}
