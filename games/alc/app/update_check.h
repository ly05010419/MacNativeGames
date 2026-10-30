#pragma once
#import <Foundation/Foundation.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <math.h>

// Public GitHub metadata only; never downloads or installs an update.
static inline BOOL mng_revision(id value) {
    return [value isKindOfClass:NSString.class] && [value length] == 40 &&
        [value rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet].location == NSNotFound;
}

static inline BOOL mng_relevant_path(id value) {
    if (![value isKindOfClass:NSString.class]) return NO;
    NSString *path = value;
    if ([@[@"md", @"rst"] containsObject:path.pathExtension.lowercaseString]) return NO;
    if ([path hasPrefix:@"games/totk/ios/"] || [path hasPrefix:@"games/totk/tests/"]) return NO;
    return [path hasPrefix:@"games/totk/"] || [path hasPrefix:@"vendor/"] ||
        [path isEqualToString:@"build_app.py"] || [path isEqualToString:@"Makefile"];
}

static inline BOOL mng_comparison_has_update(id value) {
    if (![value isKindOfClass:NSDictionary.class]) return NO;
    NSDictionary *d = value;
    if (!([d[@"status"] isEqual:@"ahead"] || [d[@"status"] isEqual:@"diverged"]) || ![d[@"ahead_by"] isKindOfClass:NSNumber.class] ||
        [d[@"ahead_by"] integerValue] <= 0 || ![d[@"files"] isKindOfClass:NSArray.class]) return NO;
    for (id f in d[@"files"]) {
        if (![f isKindOfClass:NSDictionary.class]) continue;
        if (mng_relevant_path(f[@"filename"]) || mng_relevant_path(f[@"previous_filename"])) return YES;
    }
    // GitHub truncates file lists at 300. Avoid silently missing changes beyond that limit.
    return [d[@"files"] count] >= 300;
}

static inline NSDictionary *mng_public_json(NSString *suffix, double deadline) {
    double remaining = deadline - NSProcessInfo.processInfo.systemUptime;
    if (remaining <= 0) return nil;
    NSURL *url = [NSURL URLWithString:[@"https://api.github.com/repos/ly05010419/MacNativeGames/" stringByAppendingString:suffix]];
    NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:remaining];
    [request setValue:@"MacNativeGames-update-notice" forHTTPHeaderField:@"User-Agent"];
    [request setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.timeoutIntervalForResource = remaining;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NSDictionary *result = nil;
    NSURLSessionDataTask *task = [session dataTaskWithRequest:request completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (!error && [response isKindOfClass:NSHTTPURLResponse.class] &&
            [(NSHTTPURLResponse *)response statusCode] == 200 && data.length <= 16 * 1024 * 1024) {
            id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
            if ([json isKindOfClass:NSDictionary.class]) result = json;
        }
        dispatch_semaphore_signal(done);
    }];
    [task resume];
    BOOL finished = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(remaining * NSEC_PER_SEC))) == 0;
    [session invalidateAndCancel];
    return finished ? result : nil;
}

static inline BOOL mng_updates_enabled(void) {
    const char *enabled = getenv("MNG_CHECK_UPDATES");
    return !enabled || strcmp(enabled, "0");
}

static inline NSString *mng_update_state_path(NSString *folder) {
    return [folder stringByAppendingPathComponent:@"port-update-state.plist"];
}

static inline NSString *mng_update_skip_path(NSString *folder) {
    return [folder stringByAppendingPathComponent:@"port-update-skipped.txt"];
}

static inline NSString *mng_update_skipped(NSString *folder) {
    return [[NSString stringWithContentsOfFile:mng_update_skip_path(folder) encoding:NSUTF8StringEncoding error:nil]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static inline BOOL mng_update_due(id last, double now) {
    // Count failed attempts too. A clock moved backwards must not cause a retry storm.
    return ![last isKindOfClass:NSNumber.class] || !isfinite([last doubleValue]) ||
        now - [last doubleValue] >= 24 * 60 * 60;
}

static inline NSString *mng_cached_update(NSDictionary *state, NSString *base, NSString *skipped) {
    NSString *head = state[@"head"];
    if (!mng_revision(base) || ![state[@"base"] isEqual:base] || !mng_revision(head) ||
        [head isEqual:base] || [head isEqual:skipped] || [head isEqual:state[@"skipped"]]) return nil;
    return head;
}

typedef NSDictionary *(^MNGUpdateFetch)(NSString *, double);

// Only called off the main thread. Serialize overlapping runtimes without waiting,
// and persist the attempt before networking so crashes/offline launches still throttle.
static inline void mng_update_refresh(NSString *folder, NSString *base, double now, MNGUpdateFetch fetch) {
    if (!mng_updates_enabled() || !mng_revision(base) || !isfinite(now)) return;
    NSString *path = mng_update_state_path(folder);
    int lock = open([folder stringByAppendingPathComponent:@"port-update.lock"].fileSystemRepresentation,
                    O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0) return;
    if (flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return; }
    @try {
        NSMutableDictionary *state = [[NSDictionary dictionaryWithContentsOfFile:path] mutableCopy] ?: [NSMutableDictionary new];
        if (!mng_update_due(state[@"lastAttempt"], now)) return;
        state[@"lastAttempt"] = @(now);
        if (![state writeToFile:path atomically:YES]) return;
        double deadline = NSProcessInfo.processInfo.systemUptime + 3.0;
        NSDictionary *ref = fetch(@"git/ref/heads/main", deadline);
        id object = ref[@"object"];
        NSString *head = [object isKindOfClass:NSDictionary.class] ? object[@"sha"] : nil;
        if (!mng_revision(head)) return; // keep the previous successful result on transient errors
        if (![head isEqual:base]) {
            NSString *query = [NSString stringWithFormat:@"compare/%@...%@?per_page=1", base, head];
            NSDictionary *comparison = fetch(query, deadline);
            if (!comparison) return;
            if (!mng_comparison_has_update(comparison)) head = nil;
        } else head = nil;
        state[@"base"] = base;
        if (head) state[@"head"] = head;
        else [state removeObjectForKey:@"head"];
        [state writeToFile:path atomically:YES];
    } @finally { close(lock); }
}

static inline void mng_update_schedule(NSString *folder, NSString *base, MNGUpdateFetch fetch) {
    if (!mng_updates_enabled() || !folder.length || !mng_revision(base)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        @autoreleasepool { mng_update_refresh(folder, base, NSDate.date.timeIntervalSince1970, fetch); }
    });
}

static inline void mng_update_background(void) {
    const char *folder = getenv("MNG_UPDATE_DATA"), *base = getenv("MNG_UPDATE_REVISION");
    if (!folder || !base) return; // direct/unpackaged runtimes have no update metadata
    mng_update_schedule(@(folder), @(base), ^NSDictionary *(NSString *query, double deadline) {
        return mng_public_json(query, deadline);
    });
}
