/* The leaderboard (user 2026-11-11: a settings tab of its own, apart from the shaders; the switch "我要上榜" is off by
   default and nothing asks or announces it; the tab shows the latest board; with the switch on the player's own place
   is green; anonymous). Mac only: the launcher names the folder (TOTK_BOARD_DIR), the game's frontend starts the rounds
   once its window is up (totk_board_start), the 排行榜 page (board_tab.h) turns it on and off and reads the board.

   What a round does, every TOTK_BOARD_TICK seconds: when the pacing or picture settings changed, the frame record
   (fps_board.c) starts again, so a record never mixes settings; every TOTK_BOARD_SEND_EVERY rounds, with the switch on
   and at least TOTK_BOARD_MIN_SECONDS of play counted, the session so far is sent. What is sent: the game and its
   version, the app's build, the device model, chip, GPU cores, memory, system version, the pacing (frame lock,
   interpolation), the resolutions, MetalFX, the display's refresh rate, the frame numbers, and a random id of its
   own (leaderboard-id.txt, not the shader inbox's). The server returns no id to anyone; it marks the caller's row. */
#pragma once
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include "../third_party/d2r_gfx/gfx/nvn/fps_board.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h"   /* totk_inbox_build / _os / _device, GAME_INBOX_ID */

#ifndef TOTK_BOARD_URL
#define TOTK_BOARD_URL "https://mac-native-games.com/v1/board"
#endif
#define TOTK_BOARD_TICK 60          /* seconds between rounds */
#define TOTK_BOARD_SEND_EVERY 5     /* rounds between sends: every five minutes */
#define TOTK_BOARD_MIN_SECONDS 120  /* counted play a record needs (the server lists no shorter one) */
#define TOTK_BOARD_TIMEOUT 30.0

/* Tests only: TOTK_BOARD_URL points the board at a local server; TOTK_BOARD_TEST=1 makes a round every 10 s, sends every
   round and needs 20 s of play. */
static inline NSURL *totk_board_url(void) {
    const char *e = getenv("TOTK_BOARD_URL");
    return [NSURL URLWithString:e && *e ? @(e) : @TOTK_BOARD_URL];
}
static inline BOOL totk_board_test(void) { const char *e = getenv("TOTK_BOARD_TEST"); return e && atoi(e) > 0; }
static inline int totk_board_tick(void) { return totk_board_test() ? 10 : TOTK_BOARD_TICK; }
static inline unsigned totk_board_send_every(void) { return totk_board_test() ? 1 : TOTK_BOARD_SEND_EVERY; }
static inline double totk_board_min_seconds(void) { return totk_board_test() ? 20 : TOTK_BOARD_MIN_SECONDS; }

void nvn_board_pacing(int *lock, int *interp);                                       /* hle_nvn.c */
void mtl_board_view(int *out_w, int *out_h, int *scene_w, int *scene_h, int *metalfx); /* nvn_mtl.m */
int mtl_display_hz(void);

/* The player's data folder (the launcher sets TOTK_BOARD_DIR); nil in a development run, which takes no part. */
static inline NSString *totk_board_dir(void) {
    const char *e = getenv("TOTK_BOARD_DIR");
    return e && *e ? @(e) : nil;
}
static inline NSString *totk_board_file(NSString *name) {
    NSString *dir = totk_board_dir();
    return dir ? [dir stringByAppendingPathComponent:name] : nil;
}

static inline BOOL totk_board_on(void) {
    NSString *file = totk_board_file(@"leaderboard.txt");
    NSString *v = file ? [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil] : nil;
    return [[v stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] isEqualToString:@"on"];
}
static inline BOOL totk_board_set(BOOL on) {
    NSString *file = totk_board_file(@"leaderboard.txt");
    NSError *error = nil;
    if (!file || ![(on ? @"on\n" : @"off\n") writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
        fprintf(stderr, "[board] the choice was not saved: %s\n", error ? error.localizedDescription.UTF8String : "no data folder");
        return NO;
    }
    fprintf(stderr, "[board] 我要上榜 %s\n", on ? "on" : "off");
    return YES;
}

/* The board's own random id, made once (lowercase, as the server takes it). */
static inline NSString *totk_board_id(BOOL make) {
    NSString *file = totk_board_file(@"leaderboard-id.txt");
    if (!file) return nil;
    NSString *v = [[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil]
                   stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (v.length == 36) return v;
    if (!make) return nil;
    NSString *id_ = NSUUID.UUID.UUIDString.lowercaseString;
    return [id_ writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil] ? id_ : nil;
}

static inline NSString *totk_board_sysctl(const char *name) {
    char value[128] = {0}; size_t size = sizeof value - 1;
    return sysctlbyname(name, value, &size, NULL, 0) == 0 && value[0] ? @(value) : nil;
}
/* The GPU's core count from its registry entry (AGXAccelerator "gpu-core-count"); 0 if not found. */
static inline int totk_board_gpu_cores(void) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    if (!s) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
    IOObjectRelease(s);
    int n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
    if (v) CFRelease(v);
    return n;
}
/* Chip, GPU cores, memory: read once. */
static inline NSDictionary *totk_board_hardware(void) {
    static NSDictionary *hw;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        NSString *chip = totk_board_sysctl("machdep.cpu.brand_string");
        if (chip) d[@"chip"] = [chip substringToIndex:MIN(chip.length, (NSUInteger)64)];
        int cores = totk_board_gpu_cores();
        if (cores > 0) d[@"gpu_cores"] = @(cores);
        uint64_t mem = 0; size_t size = sizeof mem;
        if (sysctlbyname("hw.memsize", &mem, &size, NULL, 0) == 0 && mem) d[@"memory_gb"] = @((int)llround((double)mem / (1ull << 30)));
        hw = [d copy];
    });
    return hw;
}

/* The pacing and picture settings now. */
static inline NSDictionary *totk_board_settings(void) {
    int lock = 0, interp = 0, ow = 0, oh = 0, sw = 0, sh = 0, mfx = 0;
    nvn_board_pacing(&lock, &interp);
    mtl_board_view(&ow, &oh, &sw, &sh, &mfx);
    return @{@"lock": @(lock), @"interp": @(interp), @"out_w": @(ow), @"out_h": @(oh), @"scene_w": @(sw), @"scene_h": @(sh),
             @"metalfx": @((BOOL)(mfx != 0)), @"display_hz": @(MAX(mtl_display_hz(), 0))};   /* BOOL: JSON true / false, not 1 / 0 */
}

/* One fps number as the server takes it: one decimal. */
static inline NSNumber *totk_board_fps(double v) { return @(round(v * 10) / 10); }

/* The record to send, or nil when there is not enough play yet or a size is not known. */
static inline NSDictionary *totk_board_body(NSString *board_id, NSDictionary *settings) {
    fps_board_stats s;
    if (!board_id || !fps_board_snapshot(&s) || s.seconds < totk_board_min_seconds()) return nil;
    for (NSString *k in @[@"out_w", @"out_h", @"scene_w", @"scene_h"]) if ([settings[k] intValue] <= 0) return nil;
    NSMutableDictionary *body = [@{@"schema": @1, @"game": @GAME_INBOX_ID, @"board_id": board_id,
                                   @"game_version": @TOTK_GAME_VERSION, @"app_build": totk_inbox_build(), @"platform": @"mac",
                                   @"os": totk_inbox_os(), @"settings": settings,
                                   @"stats": @{@"seconds": @(round(s.seconds * 10) / 10), @"frames": @(s.frames),
                                               @"avg": totk_board_fps(s.avg), @"median": totk_board_fps(s.median),
                                               @"low1": totk_board_fps(s.low1), @"max": totk_board_fps(s.max),
                                               @"interp_fps": totk_board_fps(s.interp_fps)}} mutableCopy];
    NSString *device = totk_inbox_device();
    if (device) body[@"device"] = device;
    [body addEntriesFromDictionary:totk_board_hardware()];
    return body;
}

static inline void totk_board_post(NSDictionary *body) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if (!json) return;
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:totk_board_url()
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:TOTK_BOARD_TIMEOUT];
    request.HTTPMethod = @"POST";
    request.HTTPBody = json;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        if (error) fprintf(stderr, "[board] not sent: %s\n", error.localizedDescription.UTF8String);
        else if (status != 200) fprintf(stderr, "[board] HTTP %ld: %.*s\n", (long)status, (int)MIN(data.length, (NSUInteger)300), (const char *)data.bytes);
        else fprintf(stderr, "[board] sent %.0f s of play\n", [body[@"stats"][@"seconds"] doubleValue]);
    }] resume];
}

/* The settings a record must not mix: the internal size is left out, since the game may scale it in play (it is sent as
   last seen). */
static inline NSDictionary *totk_board_fixed(NSDictionary *settings) {
    NSMutableDictionary *d = [settings mutableCopy];
    [d removeObjectsForKeys:@[@"scene_w", @"scene_h"]];
    return d;
}

/* One round (on the board's own queue). */
static inline void totk_board_round(void) {
    static NSDictionary *session;
    static unsigned rounds;
    NSDictionary *settings = totk_board_settings(), *now = totk_board_fixed(settings);
    if (session && ![session isEqualToDictionary:now]) {
        BOOL counting = fps_board_counting();
        fps_board_reset();
        if (counting) fps_board_start();
        fprintf(stderr, "[board] settings changed: the frame record starts again\n");
    }
    session = now;
    if (++rounds % totk_board_send_every() || !totk_board_on()) return;
    NSDictionary *body = totk_board_body(totk_board_id(YES), settings);
    if (body) totk_board_post(body);
}

/* Starts the rounds once per process (frontend.m, once the window is up), when the launcher named a folder. Runs with the
   switch off too: the record follows the settings, so turning the switch on sends a clean record at the next send. */
static dispatch_source_t totk_board_timer;
static inline void totk_board_start(void) {
    if (totk_board_timer || !totk_board_dir()) return;
    dispatch_queue_t queue = dispatch_queue_create("org.macnativegames.board",
                                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_event_handler(timer, ^{ @autoreleasepool { totk_board_round(); } });
    int tick = totk_board_tick();
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)tick * NSEC_PER_SEC), (uint64_t)tick * NSEC_PER_SEC, NSEC_PER_SEC);
    totk_board_timer = timer;
    dispatch_resume(timer);
}

/* The latest board for this game (me: the player's id while the switch is on, so the server marks their row). */
static inline void totk_board_fetch(void (^done)(NSDictionary *board, NSString *error)) {
    NSURLComponents *url = [NSURLComponents componentsWithURL:totk_board_url() resolvingAgainstBaseURL:NO];
    NSMutableArray *query = [NSMutableArray arrayWithObject:[NSURLQueryItem queryItemWithName:@"game" value:@GAME_INBOX_ID]];
    NSString *me = totk_board_on() ? totk_board_id(NO) : nil;
    if (me) [query addObject:[NSURLQueryItem queryItemWithName:@"me" value:me]];
    url.queryItems = query;
    NSURLRequest *request = [NSURLRequest requestWithURL:url.URL cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:TOTK_BOARD_TIMEOUT];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        id json = status == 200 && data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
        id payload = [json isKindOfClass:NSDictionary.class] ? json[@"data"] : nil;   /* the server's envelope: {"success": true, "data": {...}} */
        NSDictionary *board = [payload isKindOfClass:NSDictionary.class] && [payload[@"groups"] isKindOfClass:NSArray.class] ? payload : nil;
        NSString *why = board ? nil : error ? error.localizedDescription : [NSString stringWithFormat:@"HTTP %ld", (long)status];
        if (why) fprintf(stderr, "[board] the board was not read: %s\n", why.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ done(board, why); });
    }] resume];
}
