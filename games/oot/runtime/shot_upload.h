/* Screenshot sharing (user 2026-11-11: a settings tab of its own, 截图分享; the switch is off by default and nothing asks
   or announces it; with it on, every screenshot the capture button saves is shared, no question per picture; anonymous:
   only the picture and the board's random id leave this Mac; the player can take back their own; Mac only; the picture
   at most 1920 on its long edge, JPEG quality 85). The launcher names the folder (TOTK_BOARD_DIR, the leaderboard's), the
   frontend points screenshot.h's totk_screenshot_shared at totk_shot_queue and calls totk_shot_start once its window is
   up, the 截图分享 page turns it on and off, lists the game's shared pictures and deletes the player's own.

   A picture, on the shots' own queue: drawn into sRGB at most TOTK_SHOT_LONG_EDGE on its long edge (never larger than it
   was), JPEG at TOTK_SHOT_QUALITY, a thumbnail of TOTK_SHOT_THUMB_EDGE at TOTK_SHOT_THUMB_QUALITY (lower when it does not
   fit the server's limit), and the exact upload body is kept in shot-pending/<unix ms>.json beside the switch before it
   is sent, so a picture taken offline or past the day's limit goes on a later try (TOTK_SHOT_FIRST seconds after the next
   start, or with the next picture). The server takes 20 a day from a player; at most TOTK_SHOT_PENDING_MAX wait here.
   What is sent: the game's id, the board's random id (MacNativeGames/leaderboard-id.txt, one for every game of this Mac,
   board_upload.h), the JPEG, its thumbnail and its size. The server never returns the id: it marks the caller's pictures
   with "me", which is also what lets the player delete them. Turning the switch off drops what waits. */
#pragma once
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#include <sys/time.h>
#include "board_upload.h"   /* totk_board_dir, totk_board_id, GAME_INBOX_ID */

#ifndef TOTK_SHOT_URL
#define TOTK_SHOT_URL "https://mac-native-games.com/v1/shots"
#endif
#define TOTK_SHOT_SWITCH "screenshot-share.txt"   /* "on" / "off", beside leaderboard.txt */
#define TOTK_SHOT_PENDING "shot-pending"          /* the pictures waiting to be sent, one upload body each */
#define TOTK_SHOT_LONG_EDGE 1920                  /* the server's limit (shot-validate.js longEdge) */
#define TOTK_SHOT_QUALITY 0.85
#define TOTK_SHOT_IMAGE_BYTES 2500000             /* shot-validate.js imageBytes */
#define TOTK_SHOT_THUMB_EDGE 400
#define TOTK_SHOT_THUMB_QUALITY 0.7
#define TOTK_SHOT_THUMB_BYTES 80000               /* shot-validate.js thumbBytes */
#define TOTK_SHOT_PENDING_MAX 20                  /* the server's day for one player (shots.js perPlayerPerDay) */
#define TOTK_SHOT_FIRST 20                        /* seconds after start: what the last session left is sent */
#define TOTK_SHOT_TIMEOUT 60.0
#define TOTK_SHOT_THUMBS 300                      /* thumbnails kept in memory for the page */

/* Tests only: TOTK_SHOT_URL points the sharing at a local server; TOTK_SHOT_TEST=1 sends what waits 2 s after start. */
static inline NSURL *totk_shot_url(void) {
    const char *e = getenv("TOTK_SHOT_URL");
    return [NSURL URLWithString:e && *e ? @(e) : @TOTK_SHOT_URL];
}
static inline int totk_shot_first(void) { const char *e = getenv("TOTK_SHOT_TEST"); return e && atoi(e) > 0 ? 2 : TOTK_SHOT_FIRST; }

/* The shots' own serial queue: the scaling, the JPEGs, the pending files and the posts, never the game's threads. */
static inline dispatch_queue_t totk_shot_worker(void) {
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("org.macnativegames.shots", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    });
    return queue;
}

/* ---- The switch ---- */
static inline BOOL totk_shot_on(void) {
    NSString *file = totk_board_file(@TOTK_SHOT_SWITCH);
    NSString *v = file ? [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil] : nil;
    return [[v stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqualToString:@"on"];
}
static inline BOOL totk_shot_set(BOOL on) {
    NSString *file = totk_board_file(@TOTK_SHOT_SWITCH);
    NSError *error = nil;
    if (!file || ![(on ? @"on\n" : @"off\n") writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
        fprintf(stderr, "[shots] the choice was not saved: %s\n", error ? error.localizedDescription.UTF8String : "no data folder");
        return NO;
    }
    if (!on) [NSFileManager.defaultManager removeItemAtPath:totk_board_file(@TOTK_SHOT_PENDING) error:nil];   /* off: nothing waiting is sent later */
    fprintf(stderr, "[shots] 截图分享 %s\n", on ? "on" : "off");
    return YES;
}

/* ---- The picture ---- */
/* img drawn into sRGB without alpha, at most edge on its long side (the aspect kept, never enlarged); NULL when it cannot
   be drawn. The caller releases it. */
static inline CGImageRef totk_shot_scaled(CGImageRef img, size_t edge) CF_RETURNS_RETAINED {
    size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img), longest = MAX(w, h);
    if (!w || !h) return NULL;
    if (longest > edge) {
        w = MAX((size_t)1, (size_t)llround((double)w * (double)edge / (double)longest));
        h = MAX((size_t)1, (size_t)llround((double)h * (double)edge / (double)longest));
    }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = space ? CGBitmapContextCreate(NULL, w, h, 8, 0, space, (CGBitmapInfo)kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big) : NULL;
    if (space) CGColorSpaceRelease(space);
    if (!context) return NULL;
    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextDrawImage(context, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
    CGImageRef out = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    return out;
}
/* img as a JPEG of the given quality; nil when ImageIO refuses. */
static inline NSData *totk_shot_jpeg(CGImageRef img, double quality) {
    NSMutableData *data = [NSMutableData data];
    CGImageDestinationRef destination = CGImageDestinationCreateWithData((__bridge CFMutableDataRef)data, CFSTR("public.jpeg"), 1, NULL);
    if (!destination) return nil;
    NSDictionary *options = @{(__bridge NSString *)kCGImageDestinationLossyCompressionQuality: @(quality)};
    CGImageDestinationAddImage(destination, img, (__bridge CFDictionaryRef)options);
    BOOL ok = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    return ok ? data : nil;
}
/* The JPEG at quality, or lower in steps of 0.1 until it holds at most limit bytes; nil when even 0.1 does not. */
static inline NSData *totk_shot_jpeg_within(CGImageRef img, double quality, NSUInteger limit) {
    for (int step = 0; quality - 0.1 * step > 0.05; step++) {
        NSData *data = totk_shot_jpeg(img, quality - 0.1 * step);
        if (!data) return nil;
        if (data.length <= limit) return data;
    }
    return nil;
}
/* The upload body for img (shots.js POST /v1/shots), or nil with the reason logged. */
static inline NSDictionary *totk_shot_body(CGImageRef img) {
    NSString *board_id = totk_board_id(YES);
    if (!board_id) { fprintf(stderr, "[shots] not shared: no board id\n"); return nil; }
    CGImageRef full = totk_shot_scaled(img, TOTK_SHOT_LONG_EDGE), thumb = full ? totk_shot_scaled(full, TOTK_SHOT_THUMB_EDGE) : NULL;
    NSData *image = full ? totk_shot_jpeg_within(full, TOTK_SHOT_QUALITY, TOTK_SHOT_IMAGE_BYTES) : nil;
    NSData *small = thumb ? totk_shot_jpeg_within(thumb, TOTK_SHOT_THUMB_QUALITY, TOTK_SHOT_THUMB_BYTES) : nil;
    size_t width = full ? CGImageGetWidth(full) : 0, height = full ? CGImageGetHeight(full) : 0;
    if (full) CGImageRelease(full);
    if (thumb) CGImageRelease(thumb);
    if (!image || !small) { fprintf(stderr, "[shots] not shared: the picture could not be made into a JPEG\n"); return nil; }
    return @{@"schema": @1, @"game": @GAME_INBOX_ID, @"board_id": board_id,
             @"image": [image base64EncodedStringWithOptions:0], @"thumb": [small base64EncodedStringWithOptions:0],
             @"width": @(width), @"height": @(height)};
}

/* ---- What waits to be sent ---- */
/* The pending files, oldest first (their names are the unix ms they were kept at). */
static inline NSArray<NSString *> *totk_shot_pending_files(void) {
    NSString *dir = totk_board_file(@TOTK_SHOT_PENDING);
    NSArray<NSString *> *names = dir ? [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] : nil;
    NSMutableArray<NSString *> *files = [NSMutableArray array];
    for (NSString *name in names)
        if ([name.pathExtension isEqualToString:@"json"]) [files addObject:[dir stringByAppendingPathComponent:name]];
    return [files sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
        long long x = a.lastPathComponent.stringByDeletingPathExtension.longLongValue, y = b.lastPathComponent.stringByDeletingPathExtension.longLongValue;
        return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
    }];
}
/* Keeps body as shot-pending/<unix ms>.json (the next free ms when two come in one): YES when written. */
static inline BOOL totk_shot_keep(NSDictionary *body) {
    NSString *dir = totk_board_file(@TOTK_SHOT_PENDING);
    NSData *json = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if (!dir || !json || ![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) {
        fprintf(stderr, "[shots] the picture was not kept: %s\n", dir ? dir.UTF8String : "no data folder");
        return NO;
    }
    struct timeval now;
    gettimeofday(&now, NULL);
    long long ms = (long long)now.tv_sec * 1000 + now.tv_usec / 1000;
    NSString *file;
    while ([NSFileManager.defaultManager fileExistsAtPath:(file = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%lld.json", ms]])]) ms++;
    if (![json writeToFile:file atomically:YES]) { fprintf(stderr, "[shots] the picture was not kept: %s\n", file.UTF8String); return NO; }
    return YES;
}

/* One post, waited for (on the shots' queue): the HTTP status, 0 when no answer came. */
static inline NSInteger totk_shot_post(NSData *json) {
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:totk_shot_url() cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:TOTK_SHOT_TIMEOUT];
    request.HTTPMethod = @"POST";
    request.HTTPBody = json;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    __block NSInteger status = 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        if (error) fprintf(stderr, "[shots] not sent: %s\n", error.localizedDescription.UTF8String);
        else if (status != 200) fprintf(stderr, "[shots] HTTP %ld: %.*s\n", (long)status, (int)MIN(data.length, (NSUInteger)300), (const char *)data.bytes);
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   /* the task's own timeout ends it */
    return status;
}
/* Posted on the main queue each time the server took one of the player's pictures, so the 截图分享 page (shot_tab.h) reads
   its list at once instead of at its next minute. */
#define TOTK_SHOT_SHARED_NOTE @"TOTKShotShared"
static inline void totk_shot_announce(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ [NSNotificationCenter.defaultCenter postNotificationName:TOTK_SHOT_SHARED_NOTE object:nil]; });
}
/* Sends what waits, oldest first (on the shots' queue): taken (200) or refused for good (400 a body out of shape, 403 a
   banned player) leaves the folder; the day's limit (429), the month's pause (503), any other answer or none stops the
   round and keeps the rest for a later one. */
static inline void totk_shot_send_now(void) {
    if (!totk_shot_on()) return;
    for (NSString *file in totk_shot_pending_files()) {
        NSData *json = [NSData dataWithContentsOfFile:file];
        if (!json.length) {
            fprintf(stderr, "[shots] dropped an unreadable picture: %s\n", file.lastPathComponent.UTF8String);
            [NSFileManager.defaultManager removeItemAtPath:file error:nil];
            continue;
        }
        NSInteger status = totk_shot_post(json);
        if (status == 200 || status == 400 || status == 403) {
            [NSFileManager.defaultManager removeItemAtPath:file error:nil];
            if (status == 200) {
                fprintf(stderr, "[shots] shared %s\n", file.lastPathComponent.UTF8String);
                totk_shot_announce();   /* the 截图分享 page, if open, reads the list now */
            }
            else fprintf(stderr, "[shots] dropped %s: %s\n", file.lastPathComponent.UTF8String,
                         status == 403 ? "this player may not share" : "the server refused the picture");
            continue;
        }
        fprintf(stderr, "[shots] %s kept for a later try (%s)\n", file.lastPathComponent.UTF8String,
                status == 429 ? "the day's limit" : status == 503 ? "sharing paused this month" : status ? "the server's answer" : "no answer");
        break;
    }
}
static inline void totk_shot_send_pending(void) {
    dispatch_async(totk_shot_worker(), ^{ @autoreleasepool { totk_shot_send_now(); } });
}

/* screenshot.h's totk_screenshot_shared (the screenshot queue, a picture just saved): 1 when the picture will be shared, 0
   when the switch is off, the launcher named no folder (a development run) or TOTK_SHOT_PENDING_MAX already wait. Returns at
   once; the JPEGs are made on the shots' queue. */
static inline int totk_shot_queue(CGImageRef img) {
    if (!img || !totk_board_dir() || !totk_shot_on()) return 0;
    if (totk_shot_pending_files().count >= TOTK_SHOT_PENDING_MAX) {
        fprintf(stderr, "[shots] not shared: %d pictures wait already\n", TOTK_SHOT_PENDING_MAX);
        return 0;
    }
    CGImageRef taken = CGImageRetain(img);
    dispatch_async(totk_shot_worker(), ^{
        @autoreleasepool {
            NSDictionary *body = totk_shot_body(taken);
            CGImageRelease(taken);
            if (body && totk_shot_keep(body)) totk_shot_send_pending();
        }
    });
    return 1;
}

/* Once per process (frontend.m, once the window is up), when the launcher named a folder: TOTK_SHOT_FIRST seconds later
   what the last session left is sent (only with the switch on, which totk_shot_send_now checks then). */
static BOOL totk_shot_started;
static inline void totk_shot_start(void) {
    if (totk_shot_started || !totk_board_dir()) return;
    totk_shot_started = YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)totk_shot_first() * NSEC_PER_SEC), totk_shot_worker(), ^{ @autoreleasepool { totk_shot_send_now(); } });
}

/* ---- The page's reads ---- */
/* A shot's own address (/v1/shots/<id>) with a query; nil for an id that is not the server's (digits only). */
static inline NSURL *totk_shot_item_url(NSString *shotId, NSString *name, NSString *value) {
    if (!shotId.length || shotId.length > 18 || [shotId rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location != NSNotFound) return nil;
    NSURLComponents *url = [NSURLComponents componentsWithURL:[totk_shot_url() URLByAppendingPathComponent:shotId] resolvingAgainstBaseURL:NO];
    url.queryItems = @[[NSURLQueryItem queryItemWithName:name value:value]];
    return url.URL;
}

/* The game's shared pictures, newest first, older than before (nil: the newest); data is the server's {game, shots:[{id,
   name, at, w, h, me}], next}. me: the player's id while the switch is on, so the server marks their own (no id is made
   just to read). The answer comes on the main queue. */
static inline void totk_shot_fetch(NSString *before, void (^done)(NSDictionary *data, NSString *error)) {
    NSURLComponents *url = [NSURLComponents componentsWithURL:totk_shot_url() resolvingAgainstBaseURL:NO];
    NSMutableArray *query = [NSMutableArray arrayWithObject:[NSURLQueryItem queryItemWithName:@"game" value:@GAME_INBOX_ID]];
    NSString *me = totk_shot_on() ? totk_board_id(NO) : nil;
    if (me) [query addObject:[NSURLQueryItem queryItemWithName:@"me" value:me]];
    if (before.length) [query addObject:[NSURLQueryItem queryItemWithName:@"before" value:before]];
    url.queryItems = query;
    NSURLRequest *request = [NSURLRequest requestWithURL:url.URL cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:TOTK_SHOT_TIMEOUT];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        id json = status == 200 && data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        id payload = [json isKindOfClass:NSDictionary.class] ? json[@"data"] : nil;   /* the server's envelope: {"success": true, "data": {...}} */
        NSDictionary *list = [payload isKindOfClass:NSDictionary.class] && [payload[@"shots"] isKindOfClass:NSArray.class] ? payload : nil;
        NSString *why = list ? nil : error ? error.localizedDescription : [NSString stringWithFormat:@"HTTP %ld", (long)status];
        if (why) fprintf(stderr, "[shots] the list was not read: %s\n", why.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ done(list, why); });
    }] resume];
}

/* The thumbnails already read, by id (they never change: a picture is only ever deleted). */
static inline NSCache<NSString *, NSImage *> *totk_shot_thumbs(void) {
    static NSCache *cache;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ cache = [NSCache new]; cache.countLimit = TOTK_SHOT_THUMBS; });
    return cache;
}
/* One picture (thumb: the small one, kept in memory), nil when it could not be read; on the main queue. */
static inline void totk_shot_image(NSString *shotId, BOOL thumb, void (^done)(NSImage *img)) {
    NSImage *cached = thumb && shotId ? [totk_shot_thumbs() objectForKey:shotId] : nil;
    NSURL *url = cached ? nil : totk_shot_item_url(shotId, @"size", thumb ? @"thumb" : @"full");
    if (!url) { dispatch_async(dispatch_get_main_queue(), ^{ done(cached); }); return; }
    NSURLRequest *request = [NSURLRequest requestWithURL:url cachePolicy:NSURLRequestUseProtocolCachePolicy timeoutInterval:TOTK_SHOT_TIMEOUT];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        NSImage *img = status == 200 && data.length ? [[NSImage alloc] initWithData:data] : nil;
        if (!img) fprintf(stderr, "[shots] picture %s was not read: %s\n", shotId.UTF8String,
                          error ? error.localizedDescription.UTF8String : [NSString stringWithFormat:@"HTTP %ld", (long)status].UTF8String);
        else if (thumb) [totk_shot_thumbs() setObject:img forKey:shotId];
        dispatch_async(dispatch_get_main_queue(), ^{ done(img); });
    }] resume];
}

/* Takes back one of the player's own pictures (me: the board id; the server answers 404 for anyone else's); ok on the
   main queue. */
static inline void totk_shot_delete(NSString *shotId, void (^done)(BOOL ok)) {
    NSString *me = totk_board_id(NO);
    NSURL *url = me ? totk_shot_item_url(shotId, @"me", me) : nil;
    if (!url) { dispatch_async(dispatch_get_main_queue(), ^{ done(NO); }); return; }
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:TOTK_SHOT_TIMEOUT];
    request.HTTPMethod = @"DELETE";
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        if (status == 200) { [totk_shot_thumbs() removeObjectForKey:shotId]; fprintf(stderr, "[shots] deleted %s\n", shotId.UTF8String); }
        else if (error) fprintf(stderr, "[shots] not deleted: %s\n", error.localizedDescription.UTF8String);
        else fprintf(stderr, "[shots] not deleted: HTTP %ld: %.*s\n", (long)status, (int)MIN(data.length, (NSUInteger)300), (const char *)data.bytes);
        dispatch_async(dispatch_get_main_queue(), ^{ done(status == 200); });
    }] resume];
}
#endif   /* !TARGET_OS_IPHONE */
