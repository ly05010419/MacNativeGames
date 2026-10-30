/* The keyboard icons mod (Mac): <data>/mods/<version>-keyboard, which the launcher (app/launcher.m) loads in place of
   <data>/mods/<version> when no controller is connected at start. The settings panel's Apply button icons
   (runtime/icons_apply.m) makes one from a copy of <version>, so it records in made-from.txt the SHA-256 of the
   manifest.tsv of the <version> it copied ("-" when there was none): once that manifest changes (a mod added or
   removed) the copy is stale, and loading it would leave the new mods out or stop the game on a manifest that no
   longer matches. A keyboard icons mod without made-from.txt was made by hand and is never stale. */
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>

#define KEYBOARD_MODS_MADE_FROM @"made-from.txt"

/* The SHA-256 (lowercase hex) of <mods>/manifest.tsv, or "-" when there is none. */
static inline NSString *keyboard_mods_source(NSString *mods) {
    NSData *data = [NSData dataWithContentsOfFile:[mods stringByAppendingPathComponent:@"manifest.tsv"]];
    if (!data) return @"-";
    unsigned char sha[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, sha);
    NSMutableString *hex = [NSMutableString string];
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [hex appendFormat:@"%02x", sha[i]];
    return hex;
}

/* 1 when <mods>-keyboard records another <mods> than the one there now. */
static inline int keyboard_mods_stale(NSString *mods) {
    NSString *made = [[mods stringByAppendingString:@"-keyboard"] stringByAppendingPathComponent:KEYBOARD_MODS_MADE_FROM];
    NSString *recorded = [NSString stringWithContentsOfFile:made encoding:NSUTF8StringEncoding error:nil];
    if (!recorded) return 0;
    return ![[recorded stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
             isEqualToString:keyboard_mods_source(mods)];
}
