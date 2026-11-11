/* The leaderboard (user 2026-11-11: a settings tab of its own, apart from the shaders; the switch "我要上榜" is off by
   default and nothing asks or announces it; the tab shows the latest board; with the switch on the player's own place
   is green; anonymous). Mac only: the launcher names the folder (TOTK_BOARD_DIR), the game's frontend starts the rounds
   once its window is up (totk_board_start), the 排行榜 page (board_tab.h) turns it on and off and reads the board.

   With the switch on, the session is kept on this Mac every five minutes and sent only at start and every half hour
   (user 2026-11-11: "只本地每五分钟记录一次，上传只是开机上传和每半小时上传"). A round, every TOTK_BOARD_TICK seconds:
   when the pacing or picture settings changed, the session so far is kept and the frame record (fps_board.c) starts
   again, so a record never mixes settings; every TOTK_BOARD_SAVE_EVERY rounds the session is kept
   (leaderboard-pending.json beside the switch, one record per frame lock and interpolation mode, once it has
   TOTK_BOARD_MIN_SECONDS of play); every TOTK_BOARD_SEND_EVERY rounds, and TOTK_BOARD_FIRST seconds after start (what
   the last session left), the kept records are sent and the ones taken leave the file. Turning the switch off drops
   them. What is sent: the game and its version, the app's build, the device model, chip, GPU cores, memory, system
   version, the pacing (frame lock, interpolation), the resolutions, MetalFX, the display's refresh rate, the frame
   numbers, and a random id of its own, one for every game of this Mac (MacNativeGames/leaderboard-id.txt, not the
   shader inbox's; user: one player is one player, so the three Zelda games share it). The server returns no id to
   anyone: it marks the caller's row and shows each player by an anonymous name made from the id. Only uncapped play
   without interpolation is kept (user 2026-11-11: a frame lock says what the player chose, not how fast the Mac is). */
#pragma once
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <sys/sysctl.h>
#include "../third_party/d2r_gfx/gfx/nvn/fps_board.h"
#include "../third_party/d2r_gfx/gfx/nvn/totk_contrib_upload.h"   /* totk_inbox_build / _os / _device, GAME_INBOX_ID */
#include "../third_party/d2r_gfx/gfx/nvn/include/totk_ui_lang.h"   /* totk_ui_zh: a refused nickname's text */

#ifndef TOTK_BOARD_URL
#define TOTK_BOARD_URL "https://mac-native-games.com/v1/board"
#endif
#define TOTK_BOARD_TICK 60          /* seconds between rounds */
#define TOTK_BOARD_SAVE_EVERY 5     /* rounds between keeping the session on this Mac: five minutes */
#define TOTK_BOARD_SEND_EVERY 30    /* rounds between sends: half an hour */
#define TOTK_BOARD_FIRST 20         /* seconds after start: what the last session kept is sent */
#define TOTK_BOARD_MIN_SECONDS 120  /* counted play a record needs (the server lists no shorter one) */
#define TOTK_BOARD_TIMEOUT 30.0

/* Tests only: TOTK_BOARD_URL points the board at a local server; TOTK_BOARD_TEST=1 makes a round every 10 s, keeps the
   session every round, sends every third round and 10 s after start, and needs 20 s of play. */
static inline NSURL *totk_board_url(void) {
    const char *e = getenv("TOTK_BOARD_URL");
    return [NSURL URLWithString:e && *e ? @(e) : @TOTK_BOARD_URL];
}
static inline BOOL totk_board_test(void) { const char *e = getenv("TOTK_BOARD_TEST"); return e && atoi(e) > 0; }
static inline int totk_board_tick(void) { return totk_board_test() ? 10 : TOTK_BOARD_TICK; }
static inline unsigned totk_board_save_every(void) { return totk_board_test() ? 1 : TOTK_BOARD_SAVE_EVERY; }
static inline unsigned totk_board_send_every(void) { return totk_board_test() ? 3 : TOTK_BOARD_SEND_EVERY; }
static inline int totk_board_first(void) { return totk_board_test() ? 10 : TOTK_BOARD_FIRST; }
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
    if (!on) [NSFileManager.defaultManager removeItemAtPath:totk_board_file(@"leaderboard-pending.json") error:nil];   /* off: nothing kept is sent later */
    fprintf(stderr, "[board] 我要上榜 %s\n", on ? "on" : "off");
    return YES;
}

static inline NSString *totk_board_read_id(NSString *file) {
    NSString *v = [[NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil]
                   stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return v.length == 36 ? v : nil;
}
/* The board's own random id, one for every game of this Mac, made once (lowercase, as the server takes it), in the
   folder above the game's (MacNativeGames/leaderboard-id.txt). An id an earlier build made in the game's own folder is
   moved up and kept, so the player's name does not change. */
static inline NSString *totk_board_id(BOOL create) {
    NSString *dir = totk_board_dir();
    if (!dir) return nil;
    NSString *file = [dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"leaderboard-id.txt"];
    NSString *v = totk_board_read_id(file), *old = [dir stringByAppendingPathComponent:@"leaderboard-id.txt"];
    if (v) return v;
    if ((v = totk_board_read_id(old)) && [v writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil]) {
        [NSFileManager.defaultManager removeItemAtPath:old error:nil];
        return v;
    }
    if (!create) return v;
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

/* One post, waited for (on the board's own queue): YES when the server took it. */
static inline BOOL totk_board_post(NSDictionary *body) {
    NSData *json = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
    if (!json) return NO;
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:totk_board_url()
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:TOTK_BOARD_TIMEOUT];
    request.HTTPMethod = @"POST";
    request.HTTPBody = json;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    __block NSInteger status = 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        if (error) fprintf(stderr, "[board] not sent: %s\n", error.localizedDescription.UTF8String);
        else if (status != 200) fprintf(stderr, "[board] HTTP %ld: %.*s\n", (long)status, (int)MIN(data.length, (NSUInteger)300), (const char *)data.bytes);
        else fprintf(stderr, "[board] sent %.0f s of play\n", [body[@"stats"][@"seconds"] doubleValue]);
        dispatch_semaphore_signal(done);
    }] resume];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);   /* the task's own timeout ends it */
    return status == 200;
}

/* The records kept on this Mac (leaderboard-pending.json): {"<lock>/<interp>": record}. */
static inline NSMutableDictionary *totk_board_pending(void) {
    NSString *file = totk_board_file(@"leaderboard-pending.json");
    NSData *data = file ? [NSData dataWithContentsOfFile:file] : nil;
    id json = data ? [NSJSONSerialization JSONObjectWithData:data options:NSJSONReadingMutableContainers error:nil] : nil;
    return [json isKindOfClass:NSMutableDictionary.class] ? json : [NSMutableDictionary dictionary];
}
static inline void totk_board_write_pending(NSDictionary *pending) {
    NSString *file = totk_board_file(@"leaderboard-pending.json");
    if (!file) return;
    if (!pending.count) { [NSFileManager.defaultManager removeItemAtPath:file error:nil]; return; }
    NSData *data = [NSJSONSerialization dataWithJSONObject:pending options:0 error:nil];
    if (!data || ![data writeToFile:file atomically:YES]) fprintf(stderr, "[board] the session was not kept: %s\n", file.UTF8String);
}
/* Only uncapped play without interpolation is ranked, so only it is kept and sent. */
static inline BOOL totk_board_ranked(NSDictionary *settings) {
    return [settings[@"lock"] intValue] == 0 && [settings[@"interp"] intValue] == 0;
}
/* Keeps the session so far (with the switch on, uncapped, and enough play), replacing the record kept before. */
static inline void totk_board_keep(NSDictionary *settings) {
    if (!settings || !totk_board_on() || !totk_board_ranked(settings)) return;
    NSDictionary *body = totk_board_body(totk_board_id(YES), settings);
    if (!body) return;
    NSMutableDictionary *pending = totk_board_pending();
    pending[[NSString stringWithFormat:@"%@/%@", settings[@"lock"], settings[@"interp"]]] = body;
    totk_board_write_pending(pending);
    fprintf(stderr, "[board] kept %.0f s of play\n", [body[@"stats"][@"seconds"] doubleValue]);
}
/* Sends what is kept; the records the server took leave the file. */
static inline void totk_board_send_kept(void) {
    if (!totk_board_on()) return;
    NSMutableDictionary *pending = totk_board_pending();
    for (NSString *key in pending.allKeys) {
        NSDictionary *record = [pending[key] isKindOfClass:NSDictionary.class] ? pending[key] : nil;
        if (!record || !totk_board_ranked(record[@"settings"])) [pending removeObjectForKey:key];   /* an earlier build kept locked play */
        else if (totk_board_post(record)) [pending removeObjectForKey:key];
    }
    totk_board_write_pending(pending);
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
    static NSDictionary *session, *sessionSettings;
    static unsigned rounds;
    NSDictionary *settings = totk_board_settings(), *now = totk_board_fixed(settings);
    if (session && ![session isEqualToDictionary:now]) {
        totk_board_keep(sessionSettings);   /* the session of the old settings is kept before the record starts again */
        BOOL counting = fps_board_counting();
        fps_board_reset();
        if (counting) fps_board_start();
        fprintf(stderr, "[board] settings changed: the frame record starts again\n");
    }
    session = now; sessionSettings = settings;
    ++rounds;
    if (rounds % totk_board_save_every() == 0) totk_board_keep(settings);
    if (rounds % totk_board_send_every() == 0) totk_board_send_kept();
}

/* Starts the rounds once per process (frontend.m, once the window is up), when the launcher named a folder, and sends
   what the last session kept TOTK_BOARD_FIRST seconds later. Runs with the switch off too: the record follows the
   settings, so turning the switch on keeps a clean record at the next keep. */
static dispatch_source_t totk_board_timer;
static inline void totk_board_start(void) {
    if (totk_board_timer || !totk_board_dir()) return;
    dispatch_queue_t queue = dispatch_queue_create("org.macnativegames.board",
                                                   dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_event_handler(timer, ^{ @autoreleasepool { totk_board_round(); } });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)totk_board_first() * NSEC_PER_SEC), queue, ^{ @autoreleasepool { totk_board_send_kept(); } });
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

/* ---- The player's nickname (user 2026-11-11) ----
   One optional nickname per player, bound to the board's random id above (so the three games share it), unique on the
   site and changeable at any time; the board and the shared screenshots show it instead of the anonymous code. Setting
   one turns on no upload switch: only the id and the name are sent (PUT /v1/name {board_id, name}; an empty name clears
   it). The 性能排行榜 and 截图分享 pages show and change it (nick_row.h). Tests only: TOTK_NAME_URL names a local server. */
#ifndef TOTK_NAME_URL
#define TOTK_NAME_URL "https://mac-native-games.com/v1/name"
#endif
#define TOTK_NAME_TIMEOUT 30.0
static inline NSURL *totk_name_url(void) {
    const char *e = getenv("TOTK_NAME_URL");
    return [NSURL URLWithString:e && *e ? @(e) : @TOTK_NAME_URL];
}
/* The server's answer: {"success": true, "data": {"name": ...}} or {"success": false, "error": "<reason>"}. */
static inline NSDictionary *totk_name_answer(NSData *data) {
    id json = data.length ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [json isKindOfClass:NSDictionary.class] ? json : nil;
}
static inline NSString *totk_name_of(NSDictionary *answer) {
    id payload = answer[@"data"];
    id name = [payload isKindOfClass:NSDictionary.class] ? payload[@"name"] : nil;
    return [name isKindOfClass:NSString.class] && [name length] ? name : nil;
}

/* The player's nickname (nil: none). No id yet means no nickname: nothing is read and none is made. error: why it could
   not be read. On the main queue. */
static inline void totk_name_fetch(void (^done)(NSString *name, NSString *error)) {
    NSString *me = totk_board_id(NO);
    if (!me) { dispatch_async(dispatch_get_main_queue(), ^{ done(nil, nil); }); return; }
    NSURLComponents *url = [NSURLComponents componentsWithURL:totk_name_url() resolvingAgainstBaseURL:NO];
    url.queryItems = @[[NSURLQueryItem queryItemWithName:@"me" value:me]];
    NSURLRequest *request = [NSURLRequest requestWithURL:url.URL cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:TOTK_NAME_TIMEOUT];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        NSDictionary *answer = status == 200 ? totk_name_answer(data) : nil;
        BOOL read = [answer[@"data"] isKindOfClass:NSDictionary.class];
        NSString *name = read ? totk_name_of(answer) : nil;
        NSString *why = read ? nil : error ? error.localizedDescription : [NSString stringWithFormat:@"HTTP %ld", (long)status];
        if (why) fprintf(stderr, "[name] the nickname was not read: %s\n", why.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{ done(name, why); });
    }] resume];
}

/* Sets the player's nickname, or clears it (nil or empty). stored: the server's form of it (trimmed, nil once cleared);
   error: the server's reason (400 / 403 / 409 / 429) or why no answer came; status: the HTTP status, 0 without an
   answer, -1 without an id (no data folder). Makes the id when there is none yet. On the main queue. */
static inline void totk_name_set(NSString *name, void (^done)(NSString *stored, NSString *error, NSInteger status)) {
    NSString *board_id = totk_board_id(YES);
    if (!board_id) { dispatch_async(dispatch_get_main_queue(), ^{ done(nil, @"no board id", -1); }); return; }
    NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"board_id": board_id, @"name": name.length ? name : NSNull.null} options:0 error:nil];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:totk_name_url() cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:TOTK_NAME_TIMEOUT];
    request.HTTPMethod = @"PUT";
    request.HTTPBody = json;
    [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    [[NSURLSession.sharedSession dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSInteger status = error ? 0 : [(NSHTTPURLResponse *)response statusCode];
        NSDictionary *answer = totk_name_answer(data);
        NSString *stored = status == 200 ? totk_name_of(answer) : nil;
        NSString *why = status == 200 ? nil : error ? error.localizedDescription
                      : [answer[@"error"] isKindOfClass:NSString.class] ? answer[@"error"] : [NSString stringWithFormat:@"HTTP %ld", (long)status];
        if (why) fprintf(stderr, "[name] the nickname was not set (%ld): %s\n", (long)status, why.UTF8String);
        else fprintf(stderr, "[name] nickname %s\n", stored ? "set" : "cleared");
        dispatch_async(dispatch_get_main_queue(), ^{ done(stored, why, status); });
    }] resume];
}

/* A refused change as the player reads it (the server's reasons, lib/name-validate.js and lib/names.js). */
static inline NSString *totk_name_problem(NSInteger status, NSString *reason) {
    BOOL zh = totk_ui_zh();
#define TOTK_NAME_TEXT(z, e) (zh ? @z : @e)
    if (status == 409) return TOTK_NAME_TEXT("这个昵称已被占用", "this nickname is taken");
    if (status == 429) return TOTK_NAME_TEXT("改得太频繁，请稍后再试", "too many changes, try again later");
    if (status == 403) return TOTK_NAME_TEXT("这个玩家不能设置昵称", "this player may not set a nickname");
    if (status == 0) return TOTK_NAME_TEXT("网络错误，请稍后再试", "network error, try again later");
    if (status < 0) return TOTK_NAME_TEXT("这次运行不能设置昵称", "this run cannot set a nickname");
    if (status == 400 && [reason containsString:@"2 to 16"]) return TOTK_NAME_TEXT("长度要 2 到 16 个字", "it must be 2 to 16 characters");
    if (status == 400 && [reason containsString:@"characters that are not allowed"])
        return TOTK_NAME_TEXT("只能用字母、汉字、数字、空格和 _ - ·", "only letters, digits, spaces and _ - · are allowed");
    if (status == 400 && [reason containsString:@"anonymous code"]) return TOTK_NAME_TEXT("不能像匿名代号（6 位 0-9 a-f）", "it must not look like an anonymous code");
    if (status == 400 && [reason containsString:@"not allowed"]) return TOTK_NAME_TEXT("这个昵称不允许使用", "this nickname is not allowed");
    return [NSString stringWithFormat:TOTK_NAME_TEXT("服务器拒绝（HTTP %ld），请稍后再试", "refused (HTTP %ld), try again later"), (long)status];
#undef TOTK_NAME_TEXT
}
