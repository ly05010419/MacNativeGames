// Active players (user 2026-11-19: "统计 每日活跃用户 … 日活跃 周活跃 月活跃 流失率"; on by default, can be turned off in the
// settings). Each program reports at most once a day (UTC) to the site, which counts daily / weekly / monthly active
// installs and the churn (MacNativeGamesWeb shader-inbox/lib/active.js):
//   POST https://mac-native-games.com/v1/ping  {"install": <32 hex>, "app": <program>, "platform": mac|iphone|ipad, "version": ...}
// Nothing else is sent: no name, no account, no file of the player's; the server keeps the install only as a salted hash
// and no address.
//   install   Mac: a random id in ~/Library/Application Support/MacNativeGames/install-id.txt, one for the Launcher and
//             every game of the Mac (the Launcher's Swift side reads and writes the same file). iPhone / iPad: a hash of
//             the vendor id (identifierForVendor), the same for every app of one team on the device.
//   off       Mac: the file usage-stats-off beside it (the Launcher's settings make and remove it; every program reads
//             it). iPhone / iPad: the app's NSUserDefaults "TOTK_USAGE_STATS" set to NO (its settings).
// A report is sent from a background queue MNG_ACTIVE_DELAY_S seconds after the start and never retried in the session:
// when it fails (offline, the site blocked) the day is not marked and the next start tries again. The day it went out
// is kept per program (Mac: usage-sent/<app>.txt; iOS: NSUserDefaults "MNGUsageSent.<app>").
//
// Every game carries a copy of this file (tests/test_active_players.py: all copies the same bytes).
#pragma once
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif

#define MNG_ACTIVE_URL "https://mac-native-games.com/v1/ping"
#define MNG_ACTIVE_DELAY_S 10

static inline NSString *mng_active_hex(const unsigned char *b, size_t n) {
    NSMutableString *s = [NSMutableString stringWithCapacity:n * 2];
    for (size_t i = 0; i < n; i++) [s appendFormat:@"%02x", b[i]];
    return s;
}
static inline int mng_active_is_id(NSString *s) {
    if (s.length != 32) return 0;
    for (NSUInteger i = 0; i < 32; i++) { unichar c = [s characterAtIndex:i]; if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0; }
    return 1;
}
#if !TARGET_OS_IPHONE
/* The folder every MacNativeGames program of this Mac shares (MNG_HOME overrides it: the tests). */
static inline NSString *mng_active_dir(void) {
    const char *home = getenv("MNG_HOME");
    return home && *home ? @(home) : [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MacNativeGames"];
}
#endif

/* 1 unless the player turned it off. */
static inline int mng_active_on(void) {
#if TARGET_OS_IPHONE
    id v = [NSUserDefaults.standardUserDefaults objectForKey:@"TOTK_USAGE_STATS"];
    return v ? [v boolValue] : 1;
#else
    return ![NSFileManager.defaultManager fileExistsAtPath:[mng_active_dir() stringByAppendingPathComponent:@"usage-stats-off"]];
#endif
}

/* The install id (32 lowercase hex), made once; nil when it cannot be had. */
static inline NSString *mng_active_install(void) {
#if TARGET_OS_IPHONE
    NSUUID *v = UIDevice.currentDevice.identifierForVendor;
    if (!v) return nil;
    NSData *d = [[@"mng-active|" stringByAppendingString:v.UUIDString] dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, md);
    return mng_active_hex(md, 16);
#else
    NSString *dir = mng_active_dir(), *file = [dir stringByAppendingPathComponent:@"install-id.txt"];
    for (int pass = 0; pass < 2; pass++) {
        NSString *v = [[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil]
                       stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        if (mng_active_is_id(v)) return v;
        if (pass) return nil;
        /* Made by whichever program comes first, and whole when it appears: the id is written to a file of this
           program's own and then linked to the name, which fails when another program linked its id first (2026-11-19:
           an empty file made with O_EXCL and written afterwards was read empty by a program starting at the same time,
           which then had no id). */
        [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        unsigned char r[16]; arc4random_buf(r, sizeof r);
        NSString *id_ = mng_active_hex(r, sizeof r);
        NSString *mine = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@".install-id.%d.%@", getpid(), id_]];
        int fd = open(mine.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) continue;
        ssize_t w = write(fd, id_.UTF8String, 32); close(fd);
        int linked = w == 32 && link(mine.fileSystemRepresentation, file.fileSystemRepresentation) == 0;
        unlink(mine.fileSystemRepresentation);
        if (linked) return id_;
    }
    return nil;
#endif
}

static inline NSString *mng_active_today(void) {
    NSDateFormatter *f = [NSDateFormatter new];
    f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"]; f.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"UTC"]; f.dateFormat = @"yyyy-MM-dd";
    return [f stringFromDate:NSDate.date];
}
static inline NSString *mng_active_sent(NSString *app) {
#if TARGET_OS_IPHONE
    return [NSUserDefaults.standardUserDefaults stringForKey:[@"MNGUsageSent." stringByAppendingString:app]];
#else
    return [NSString stringWithContentsOfFile:[[mng_active_dir() stringByAppendingPathComponent:@"usage-sent"] stringByAppendingPathComponent:[app stringByAppendingString:@".txt"]]
                                     encoding:NSUTF8StringEncoding error:nil];
#endif
}
static inline void mng_active_mark(NSString *app, NSString *day) {
#if TARGET_OS_IPHONE
    [NSUserDefaults.standardUserDefaults setObject:day forKey:[@"MNGUsageSent." stringByAppendingString:app]];
#else
    NSString *dir = [mng_active_dir() stringByAppendingPathComponent:@"usage-sent"];
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [day writeToFile:[dir stringByAppendingPathComponent:[app stringByAppendingString:@".txt"]] atomically:YES encoding:NSUTF8StringEncoding error:nil];
#endif
}
static inline NSString *mng_active_platform(void) {
#if TARGET_OS_IPHONE
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPad ? @"ipad" : @"iphone";
#else
    return @"mac";
#endif
}
/* The report's body, or nil when there is nothing to send (off, already sent today, no id). */
static inline NSData *mng_active_body(NSString *app, NSString *version, NSString *day) {
    if (!mng_active_on() || [mng_active_sent(app) isEqualToString:day]) return nil;
    NSString *install = mng_active_install();
    if (!install) return nil;
    NSDictionary *b = @{ @"install": install, @"app": app, @"platform": mng_active_platform(), @"version": version.length ? version : @"" };
    return [NSJSONSerialization dataWithJSONObject:b options:0 error:nil];
}

/* Once at the start of the program: app is its id (launcher, totk, botw, oot, …), version its version string. */
static inline void mng_active_start(NSString *app, NSString *version) {
    if (!app.length) return;
    app = [app copy]; version = [version copy] ?: @"";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)MNG_ACTIVE_DELAY_S * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ @autoreleasepool {
        NSString *day = mng_active_today();
        NSData *body = mng_active_body(app, version, day);
        if (!body) return;
        NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:@MNG_ACTIVE_URL]];
        r.HTTPMethod = @"POST"; r.HTTPBody = body; r.timeoutInterval = 20;
        [r setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
        [[NSURLSession.sharedSession dataTaskWithRequest:r completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
            (void)data;
            NSInteger code = [response isKindOfClass:NSHTTPURLResponse.class] ? ((NSHTTPURLResponse *)response).statusCode : 0;
            if (!error && code >= 200 && code < 300) mng_active_mark(app, day);
        }] resume];
    }});
}
/* The program's own version: the bundle's short version string. */
static inline NSString *mng_active_version(void) {
    id v = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return [v isKindOfClass:NSString.class] ? v : @"";
}
