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

/* At start, before the mods are chosen: a staged <mods>-keyboard.next whose drawing finished (its complete marker)
   becomes <mods>-keyboard, the one it replaces kept as <mods>-keyboard.previous (one only). A staged folder without the
   marker is left for the settings panel to replace. */
static inline void keyboard_mods_promote(NSString *mods) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSString *keyboard = [mods stringByAppendingString:@"-keyboard"], *next = [keyboard stringByAppendingString:@".next"];
    NSString *previous = [keyboard stringByAppendingString:@".previous"], *complete = [next stringByAppendingPathComponent:@"complete"];
    if (![files fileExistsAtPath:complete]) return;
    [files removeItemAtPath:previous error:nil];
    if ([files fileExistsAtPath:keyboard] && ![files moveItemAtPath:keyboard toPath:previous error:nil]) return;
    if (![files moveItemAtPath:next toPath:keyboard error:nil]) { [files moveItemAtPath:previous toPath:keyboard error:nil]; return; }
    [files removeItemAtPath:[keyboard stringByAppendingPathComponent:@"complete"] error:nil];
    fprintf(stderr, "[launcher] keyboard icons: the ones the settings panel drew are in place\n");
}

/* Whether there is a <mods>-keyboard with a manifest (the launcher asks for a controller only then). */
static inline BOOL keyboard_mods_present(NSString *mods) {
    NSString *manifest = [[mods stringByAppendingString:@"-keyboard"] stringByAppendingPathComponent:@"manifest.tsv"];
    return [NSFileManager.defaultManager fileExistsAtPath:manifest];
}

/* Whether <mods>-keyboard can be loaded: it has a manifest and is not stale. */
static inline BOOL keyboard_mods_usable(NSString *mods) {
    if (!keyboard_mods_present(mods)) return NO;
    if (!keyboard_mods_stale(mods)) return YES;
    fprintf(stderr, "[launcher] keyboard icons skipped: they predate a change to %s (apply them again in the settings panel)\n",
            mods.fileSystemRepresentation);
    return NO;
}
