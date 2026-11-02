/* The real Foundation unpacker, without the UIKit front end. No game content or device connection. */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static BOOL failAlloc, failFlush;
static void *test_malloc(size_t n) { return failAlloc ? NULL : malloc(n); }
static int test_fclose(FILE *f) {
    int result = fclose(f);
    if (failFlush) { failFlush = NO; return EOF; }
    return result;
}

#define malloc test_malloc
#define fclose test_fclose
#include PRODUCTION_UNPACKER
#undef malloc
#undef fclose

static void check(BOOL condition, const char *why) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}

static NSMutableData *pack(NSString *name, NSData *content) {
    NSMutableData *data = [NSMutableData dataWithBytes:"TOTKPACK1" length:9];
    NSData *encoded = [name dataUsingEncoding:NSUTF8StringEncoding];
    uint16_t length = encoded.length, end = 0;
    uint64_t size = content.length;
    [data appendBytes:&length length:2]; [data appendData:encoded];
    [data appendBytes:&size length:8]; [data appendData:content]; [data appendBytes:&end length:2];
    return data;
}

static void run_case(NSString *directory, NSString *label, NSMutableData *data,
                     BOOL split, BOOL oom, BOOL flush, BOOL expectOK) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *base = [directory stringByAppendingPathComponent:@"seed.pack"];
    NSString *dest = [directory stringByAppendingPathComponent:@"seed"];
    check([fm createDirectoryAtPath:dest withIntermediateDirectories:YES attributes:nil error:nil], "fixture directory");
    check([@"keep" writeToFile:[dest stringByAppendingPathComponent:@"old"] atomically:YES
                     encoding:NSUTF8StringEncoding error:nil], "fixture previous seed");
    NSUInteger part = 0;
    for (NSUInteger at = 0; at < data.length;) {
        NSUInteger count = split ? MIN((NSUInteger)7, data.length - at) : data.length - at;
        check([[data subdataWithRange:NSMakeRange(at, count)]
               writeToFile:[base stringByAppendingFormat:@".%03lu", (unsigned long)part++] atomically:YES], "fixture part");
        at += count;
    }
    failAlloc = oom; failFlush = flush;
    NSString *why = seed_unpack(base, dest, nil);
    failAlloc = failFlush = NO;
    check((why == nil) == expectOK, "unpack status");
    check(![fm fileExistsAtPath:[dest stringByAppendingString:@".tmp"]], "no incomplete seed published");
    if (expectOK) {
        check([[NSData dataWithContentsOfFile:[dest stringByAppendingPathComponent:@"shader.metal"]]
               isEqualToData:[@"synthetic payload" dataUsingEncoding:NSUTF8StringEncoding]], "exact payload");
        check(![fm fileExistsAtPath:[base stringByAppendingString:@".000"]], "completed parts removed");
    } else {
        check([[NSString stringWithContentsOfFile:[dest stringByAppendingPathComponent:@"old"]
                                         encoding:NSUTF8StringEncoding error:nil] isEqualToString:@"keep"], "previous seed preserved");
        check([fm fileExistsAtPath:[base stringByAppendingString:@".000"]], "failed parts retained for retry");
    }
    printf("PASS %s\n", label.UTF8String);
}

int main(int argc, char **argv) { @autoreleasepool {
    check(argc == 3, "case and temporary directory arguments");
    NSString *label = @(argv[1]), *directory = @(argv[2]);
    NSData *content = [@"synthetic payload" dataUsingEncoding:NSUTF8StringEncoding];
    NSMutableData *data = pack(@"shader.metal", content);
    BOOL split = NO, oom = NO, flush = NO, expectOK = NO;
    if ([label isEqualToString:@"valid_cross_part"]) { split = YES; expectOK = YES; }
    else if ([label isEqualToString:@"truncated_stream"]) data.length -= 3;
    else if ([label isEqualToString:@"bad_magic"]) ((uint8_t *)data.mutableBytes)[0] = 'X';
    else if ([label isEqualToString:@"unsafe_entry"]) data = pack(@"../escape", content);
    else if ([label isEqualToString:@"allocation_failure"]) oom = YES;
    else if ([label isEqualToString:@"final_flush_failure"]) flush = YES;
    else check(NO, "known case");
    run_case(directory, label, data, split, oom, flush, expectOK);
} return 0; }
