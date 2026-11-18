// Animal Crossing's identity at mac-native-games.com, the part of games/odyssey's totk_contrib_upload.h (copied 2026-11-17)
// that the settings panel's 性能排行榜 and 截图分享 pages need (shared/runtime/board_upload.h, shot_upload.h,
// settings_panel.h): the game's id there (GAME_INBOX_ID), the app's build, the device model and the system version that a
// board record carries, and where the board's random id is kept. Nothing here sends shaders: this game has no shader list
// for contributions to go to (games/totk/scripts/collect_shaders.py names none for it), so the panel has no 着色器贡献 page
// (runtime/settings_game.h SETTINGS_HAS_CONTRIB 0) and its contribution file stays where it is (totk_shader_contrib.h).
// Mac and iOS (Foundation only).
#pragma once
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <TargetConditionals.h>
#include "totk_shader_contrib.h"    /* totk_contrib_path: the settings panel's 访问主页 asks it (none here: no pseudonym) */
#include "include/totk_ui_lang.h"   /* TOTK_UI */

#ifndef GAME_INBOX_ID
#define GAME_INBOX_ID "acnh"   /* the game's id at the site: the one its shader seed names (tools/prepare_shader_seed.py game="acnh") */
#endif

/* Where the board's random id is kept (games/totk): MacNativeGames/leaderboard-id.txt, the folder above the game's
   (TOTK_BOARD_DIR, which the launcher names; runtime/board_upload.h totk_board_id makes and moves it). nil without that
   folder (a development run). */
static inline NSString *totk_leaderboard_id_file(void) {
    const char *e = getenv("TOTK_BOARD_DIR");
    return e && *e ? [@(e).stringByDeletingLastPathComponent stringByAppendingPathComponent:@"leaderboard-id.txt"] : nil;
}

/* The app's build as the site records it: the commit it was built from (MNGSourceCommit), else its CFBundleVersion, else
   "dev"; in the characters the site takes. */
static inline NSString *totk_inbox_build(void) {
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *v = [info[@"MNGSourceCommit"] isKindOfClass:NSString.class] ? info[@"MNGSourceCommit"] : nil;
    if (!v.length && [info[@"CFBundleVersion"] isKindOfClass:NSString.class]) v = info[@"CFBundleVersion"];
    NSString *clean = [[(v.length ? v : @"dev") componentsSeparatedByCharactersInSet:
                        [NSCharacterSet characterSetWithCharactersInString:
                         @"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._+-"].invertedSet] componentsJoinedByString:@""];
    return clean.length ? [clean substringToIndex:MIN(clean.length, (NSUInteger)64)] : @"dev";
}

/* The device's model identifier: "Mac16,1" (hw.model) on a Mac, "iPhone17,1" / "iPad16,3" (hw.machine) on iOS; nil if
   unknown. A model, not the device: every Mac of that model reports the same. */
static inline NSString *totk_inbox_device(void) {
#if TARGET_OS_IPHONE
    const char *name = "hw.machine";
#else
    const char *name = "hw.model";
#endif
    char value[64] = {0}; size_t size = sizeof value - 1;
    return sysctlbyname(name, value, &size, NULL, 0) == 0 && value[0] ? @(value) : nil;
}

/* "macOS 26.5.2", "iOS 26.1" or "iPadOS 26.1". */
static inline NSString *totk_inbox_os(void) {
    NSOperatingSystemVersion v = NSProcessInfo.processInfo.operatingSystemVersion;
#if TARGET_OS_IPHONE
    NSString *device = totk_inbox_device();
    NSString *system = [device hasPrefix:@"iPad"] ? @"iPadOS" : @"iOS";
#else
    NSString *system = @"macOS";
#endif
    return [NSString stringWithFormat:@"%@ %ld.%ld.%ld", system, (long)v.majorVersion, (long)v.minorVersion, (long)v.patchVersion];
}
