// tests/test_active_players.py: games/totk/runtime/mng_active.h on a Mac, its folder in MNG_HOME. Prints one line per check.
#import <Foundation/Foundation.h>
#include <pthread.h>
#include "mng_active.h"

static NSDictionary *parse(NSData *d) { return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil; }
static void *start_one(void *arg) { (void)arg; NSString *v = mng_active_install(); return (void *)(v ? strdup(v.UTF8String) : NULL); }

int main(void) { @autoreleasepool {
    NSString *dir = mng_active_dir(), *day = @"2026-11-19";
    // two programs starting at once keep one id (O_EXCL)
    pthread_t t[8]; char *got[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, start_one, NULL);
    int same = 1;
    for (int i = 0; i < 8; i++) { pthread_join(t[i], (void **)&got[i]); if (!got[i] || strcmp(got[i], got[0])) same = 0; }
    printf("one id for programs starting together: %d\n", same);
    NSDictionary *b = parse(mng_active_body(@"oot", @"1.0.9", day));
    printf("body keys: %s\n", [[b.allKeys sortedArrayUsingSelector:@selector(compare:)] componentsJoinedByString:@","].UTF8String);
    printf("body: app=%s platform=%s version=%s install-is-file=%d\n", [b[@"app"] UTF8String], [b[@"platform"] UTF8String], [b[@"version"] UTF8String],
           [b[@"install"] isEqualToString:[NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:@"install-id.txt"] encoding:NSUTF8StringEncoding error:nil]]);
    printf("install is 32 hex: %d\n", mng_active_is_id(b[@"install"]));
    mng_active_mark(@"oot", day);
    printf("sent today: %d, another program today: %d, tomorrow: %d\n", mng_active_body(@"oot", @"1.0.9", day) == nil,
           mng_active_body(@"launcher", @"1.0.9", day) != nil, mng_active_body(@"oot", @"1.0.9", @"2026-11-20") != nil);
    [@"off\n" writeToFile:[dir stringByAppendingPathComponent:@"usage-stats-off"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    printf("off: %d %d\n", mng_active_on(), mng_active_body(@"launcher", @"1.0.9", @"2026-11-21") != nil);
    [NSFileManager.defaultManager removeItemAtPath:[dir stringByAppendingPathComponent:@"usage-stats-off"] error:nil];
    [@"not an id" writeToFile:[dir stringByAppendingPathComponent:@"install-id.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    printf("a damaged id file gives none: %d\n", mng_active_install() == nil);
    printf("today: %ld chars\n", (long)mng_active_today().length);
    return 0;
}}
