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

/* SHA-256 of a program's code, the same whatever signed it (2026-11-18): a thin 64-bit Mach-O up to its code signature,
   with the three sizes the signature sets zeroed (__LINKEDIT's vmsize and filesize, LC_CODE_SIGNATURE's datasize);
   anything else hashed whole. MNG Launcher ships the translator with a Developer ID signature that carries a time stamp,
   so each release is another file of the same code; recorded by its whole file, a folder was set aside at every switch
   between a Launcher and a command-line update. The same as games/oot/scripts/prepare_ns2_seed.py code_sha256. */
__attribute__((unused)) static NSString *totk_code_sha256(NSString *path) {
    NSMutableData *data = [NSMutableData dataWithContentsOfFile:path];
    if (!data) return nil;
    uint8_t *b = (uint8_t *)data.mutableBytes; size_t n = data.length, keep = n;
    uint32_t magic = 0, ncmds = 0, sizeofcmds = 0;
    if (n >= 32) { memcpy(&magic, b, 4); memcpy(&ncmds, b + 16, 4); memcpy(&sizeofcmds, b + 20, 4); }
    if (magic == 0xfeedfacf && 32 + (size_t)sizeofcmds <= n) {
        size_t at = 32, end = 32 + sizeofcmds; int64_t sig = -1; BOOL ok = YES;
        for (uint32_t i = 0; i < ncmds && ok; i++) {
            uint32_t cmd, size;
            if (at + 8 > end) { ok = NO; break; }
            memcpy(&cmd, b + at, 4); memcpy(&size, b + at + 4, 4);
            if (size < 8 || at + size > end) { ok = NO; break; }
            if (cmd == 0x19 && size >= 56 && !strncmp((const char *)b + at + 8, "__LINKEDIT", 16)) { memset(b + at + 32, 0, 8); memset(b + at + 48, 0, 8); }
            else if (cmd == 0x1d && size >= 16) { uint32_t off; memcpy(&off, b + at + 8, 4); sig = off; memset(b + at + 12, 0, 4); }
            at += size;
        }
        if (ok && sig >= 0 && (size_t)sig <= n) keep = (size_t)sig;
        else data = [NSMutableData dataWithContentsOfFile:path], b = (uint8_t *)data.mutableBytes, keep = n;   // hashed whole, unchanged
    }
    uint8_t md[32]; CC_SHA256(b, (CC_LONG)keep, md);
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
/* `translator` is the program's code (totk_code_sha256) and is what the folder records; `file`, its whole-file SHA-256,
   is what folders recorded before 2026-11-18: such a record of the same file is the same translator (rewritten as code). */
__attribute__((unused)) static NSString *totk_rt_translator_check(NSString *dir, NSString *translator, NSString *file, BOOL unrecorded_untrusted, NSString **why) {
    if (why) *why = nil;
    if (translator.length != 64 || ![dir.lastPathComponent hasPrefix:@"rt-"]) return nil;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *record = [dir stringByAppendingPathComponent:@"translator.sha256"];
    NSString *had = [[NSString stringWithContentsOfFile:record encoding:NSUTF8StringEncoding error:nil]
                     stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if ([had isEqualToString:translator]) return nil;
    if (file.length == 64 && [had isEqualToString:file]) {   // recorded by the whole file before: the same program
        [[translator stringByAppendingString:@"\n"] writeToFile:record atomically:YES encoding:NSUTF8StringEncoding error:nil];
        return nil;
    }
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
