// TOTK: the contribution file sent over HTTPS to the shader inbox (a separate project, MacNativeGamesWeb/shader-inbox,
// live at mac-native-games.com), instead of a GitHub issue. One question, the same on the Mac (app/launcher.m) and the
// iPhone / iPad (ios/Sources/main.m), asked before the game starts once the file holds TOTK_CONTRIB_STEP new rows and the
// player has not chosen (user 2026-11-10: one dialog, not three; two answers): "Contribute shaders" / "Don't
// contribute" (totk_inbox_prompt, totk_inbox_answer). The choice is kept beside the file (.<name>.upload: "auto" or
// "off"). Super Mario Odyssey asks no question (its launcher is a plain C program): the settings panel's switch sets it. With "auto", a background queue sends the rows the file
// holds shortly after the game starts and then every TOTK_INBOX_EVERY seconds, at most TOTK_INBOX_CHUNK per request; the
// rows the inbox took leave the file for its done file (totk_contrib_retire), so each row is sent once. A row the inbox
// did not take (no network, a server error, too many requests) stays and goes with the next try. What is sent: the game,
// its version, the app's build, mac or ios, the device's model identifier (hw.model / hw.machine: "Mac16,1",
// "iPhone17,1") and system version, a random install id (.<name>.install, made here) and the rows, which hold shader and pipeline identifiers only. The launcher and the iOS app boot the game in their own
// process, so the queue lives as long as the game. Mac and iOS (Foundation only). Uses totk_shader_contrib.h and
// game_version.h (GAME_INBOX_ID).
#pragma once
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <TargetConditionals.h>
#include "totk_shader_contrib.h"   /* totk_contrib_read, _side, _retire, _due, _reminded, _never */
#include "include/totk_ui_lang.h"   /* TOTK_UI */

#ifndef TOTK_CONTRIB_INBOX
#define TOTK_CONTRIB_INBOX "https://mac-native-games.com/v1/contrib"   /* empty: nothing is sent and nothing is asked */
#endif
#ifndef GAME_INBOX_ID
#define GAME_INBOX_ID "odyssey"    /* the game's id in the inbox (its lib/validate.js GAMES) */
#endif
#define TOTK_INBOX_CHUNK 2000           /* rows per request (the inbox takes 5,000; a row is about 1.2 KB at most) */
#define TOTK_INBOX_FIRST 20             /* seconds after start: the game's own start-up goes first */
#define TOTK_INBOX_EVERY (30 * 60)      /* then every half hour, for the rows play adds */
#define TOTK_INBOX_TIMEOUT 60.0

static inline BOOL totk_inbox_configured(void) { return TOTK_CONTRIB_INBOX[0] != 0; }

/* The player's choice: @"auto", @"off", or nil when not asked yet. */
static inline NSString *totk_inbox_choice(NSString *path) {
    if (!path) return nil;
    NSString *v = [[NSString stringWithContentsOfFile:totk_contrib_side(path, @"upload") encoding:NSUTF8StringEncoding error:nil]
                   stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return [v isEqualToString:@"auto"] || [v isEqualToString:@"off"] ? v : nil;
}
static inline void totk_inbox_choose(NSString *path, BOOL on) {
    if (!path) return;
    NSError *error = nil;
    if (![(on ? @"auto\n" : @"off\n") writeToFile:totk_contrib_side(path, @"upload") atomically:YES encoding:NSUTF8StringEncoding error:&error])
        fprintf(stderr, "[shader-inbox] cannot keep the choice: %s\n", error.localizedDescription.UTF8String);
}

/* The one question, Mac and iOS alike: title, text and the answers in order (auto, never). */
typedef enum { TOTK_INBOX_AUTO = 0, TOTK_INBOX_NEVER = 1 } totk_inbox_answer_t;
static inline NSString *totk_inbox_prompt_title(void) {
    return @(TOTK_UI("期待你的贡献！", "We would love your contribution!"));
}
static inline NSString *totk_inbox_prompt_text(void) {
    return @(TOTK_UI(
        "选“贡献着色器”后，游戏会自动贡献着色器，不再打扰游玩体验。只发送着色器和管线的标识、游戏和 App 版本、设备型号和系统版本号，以及一个随机编号，不含游戏内容、存档或个人信息。随时可以在“设置”里关闭。",
        "With \"Contribute shaders\" the game contributes new shaders on its own and never interrupts your play. Only shader and pipeline identifiers, the game and app versions, the device model and system version, and a random id are sent: no game data, saves or personal information. You can turn it off in Settings at any time."));
}
static inline NSArray<NSString *> *totk_inbox_prompt_answers(void) {
    return @[@(TOTK_UI("贡献着色器", "Contribute shaders")), @(TOTK_UI("不贡献", "Don't contribute"))];
}

/* The rows to ask about (totk_contrib_due: TOTK_CONTRIB_STEP new rows since the last answer), or 0 when nothing is to be
   asked: no inbox, already chosen, "never", or too few rows yet. */
static inline NSUInteger totk_inbox_should_ask(NSString *path) {
    if (!path || !totk_inbox_configured() || totk_inbox_choice(path)) return 0;
    return totk_contrib_due(path);
}

/* Keeps the player's answer, either way never asked again: auto uploads from now on; never uploads nothing. */
static inline void totk_inbox_answer(NSString *path, totk_inbox_answer_t answer) {
    totk_inbox_choose(path, answer == TOTK_INBOX_AUTO);
    if (answer == TOTK_INBOX_NEVER) totk_contrib_never(path);
}

/* A random id for this contribution file, made once: the inbox counts a row once per install. Nothing about the device. */
static inline NSString *totk_inbox_install(NSString *path) {
    NSString *file = totk_contrib_side(path, @"install");
    NSString *id_ = [[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil]
                     stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (id_.length == 36) return id_;
    id_ = NSUUID.UUID.UUIDString.lowercaseString;
    return [id_ writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil] ? id_ : nil;
}

/* The app's build as the inbox records it: the commit it was built from (build_app.py MNGSourceCommit), else its
   CFBundleVersion, else "dev"; in the characters the inbox takes. */
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

/* How many rows the inbox has taken from this file (.<name>.sent, a count kept by totk_inbox_send): the settings page shows
   it as what the player has contributed. Rows the app itself serves also leave the file (the done file), so that file is
   not this number. */
static inline NSUInteger totk_inbox_sent(NSString *path) {
    NSString *v = path ? [NSString stringWithContentsOfFile:totk_contrib_side(path, @"sent") encoding:NSUTF8StringEncoding error:nil] : nil;
    return v ? (NSUInteger)strtoull(v.UTF8String, NULL, 10) : 0;
}
static inline void totk_inbox_count_sent(NSString *path, NSUInteger rows) {
    if (!path || !rows) return;
    [[NSString stringWithFormat:@"%lu\n", (unsigned long)(totk_inbox_sent(path) + rows)]
        writeToFile:totk_contrib_side(path, @"sent") atomically:YES encoding:NSUTF8StringEncoding error:nil];
}

/* One request: the HTTP status, or 0 when none came (no network, timeout). */
static inline NSInteger totk_inbox_post(NSDictionary *body) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if (!json) return 0;
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@TOTK_CONTRIB_INBOX]
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:TOTK_INBOX_TIMEOUT];
    request.HTTPMethod = @"POST";
    request.HTTPBody = json;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    __block NSInteger status = 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (error) fprintf(stderr, "[shader-inbox] not sent: %s\n", error.localizedDescription.UTF8String);
        else status = [(NSHTTPURLResponse *)response statusCode];
        if (status && status != 200)
            fprintf(stderr, "[shader-inbox] HTTP %ld: %.*s\n", (long)status, (int)MIN(data.length, (NSUInteger)300), (const char *)data.bytes);
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   // the task's own timeout ends it
    return status;
}

/* Sends what the file holds, a chunk at a time; returns the rows the inbox took. Stops at the first chunk not taken. */
static inline NSUInteger totk_inbox_send(NSString *path, NSString *platform) {
    if (!path || !totk_inbox_configured()) return 0;
    NSArray<NSString *> *rows = [totk_contrib_read(path).allObjects sortedArrayUsingSelector:@selector(compare:)];
    NSString *install = rows.count ? totk_inbox_install(path) : nil;
    if (!install) return 0;
    NSUInteger taken = 0;
    for (NSUInteger at = 0; at < rows.count; at += TOTK_INBOX_CHUNK) {
        NSArray<NSString *> *chunk = [rows subarrayWithRange:NSMakeRange(at, MIN((NSUInteger)TOTK_INBOX_CHUNK, rows.count - at))];
        NSMutableDictionary *body = [@{@"schema": @1, @"game": @GAME_INBOX_ID, @"install": install,
                                       @"game_version": @TOTK_GAME_VERSION, @"app_build": totk_inbox_build(),
                                       @"platform": platform, @"os": totk_inbox_os(), @"rows": chunk} mutableCopy];
        NSString *device = totk_inbox_device();
        if (device) body[@"device"] = device;
        NSInteger status = totk_inbox_post(body);
        if (status != 200) break;
        taken += totk_contrib_retire(path, [NSSet setWithArray:chunk]);   // malformed rows too: the inbox never takes them
    }
    if (taken) { totk_inbox_count_sent(path, taken); fprintf(stderr, "[shader-inbox] sent %lu rows\n", (unsigned long)taken); }
    return taken;
}

/* Starts the sending, once per process, when the player chose "auto" (platform: @"mac" or @"ios"); called again when the
   Settings switch turns it on during play. Each round checks the choice, so turning it off stops the sending. */
/* One timer for the whole process: the launcher and the settings panel (two files including this header) share it. */
__attribute__((weak)) dispatch_source_t totk_inbox_timer;
static inline void totk_inbox_start(NSString *path, NSString *platform) {
    if (totk_inbox_timer || !path || !totk_inbox_configured() || ![totk_inbox_choice(path) isEqualToString:@"auto"]) return;
    dispatch_queue_t queue = dispatch_queue_create("org.macnativegames.shader-inbox",
                                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    NSString *file = [path copy], *where = [platform copy];
    dispatch_source_set_event_handler(timer, ^{ @autoreleasepool {   /* the Settings switch may have turned it off since */
        if ([totk_inbox_choice(file) isEqualToString:@"auto"]) totk_inbox_send(file, where); } });
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)TOTK_INBOX_FIRST * NSEC_PER_SEC),
                              (uint64_t)TOTK_INBOX_EVERY * NSEC_PER_SEC, 60ull * NSEC_PER_SEC);
    totk_inbox_timer = timer;
    dispatch_resume(timer);
}
