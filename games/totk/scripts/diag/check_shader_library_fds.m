// Retain hundreds of libraries/functions under a 256-descriptor limit.
// No command queue or GPU rendering is created. The limit affects this process only.
// Usage: check_shader_library_fds input.metallib new-output-dir count [url]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include "totk_shader_libraries.h"

static unsigned fd_count(rlim_t limit) {
    unsigned n = 0;
    for (int fd = 0; (rlim_t)fd < limit; fd++) n += fcntl(fd, F_GETFD) >= 0;
    return n;
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "url"))) return 2;
    unsigned count = (unsigned)strtoul(argv[3], NULL, 10);
    if (!count || count > 1024) return 2;
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *source = @(argv[1]), *output = @(argv[2]);
    NSError *error = nil;
    if ([fm fileExistsAtPath:output] || ![fm createDirectoryAtPath:output withIntermediateDirectories:NO attributes:nil error:&error]) return 2;
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { puts("{\"status\":\"no-metal-device\"}"); return 77; }
    id<MTLLibrary> warm = totk_shader_library_load(device, source, &error);
    id<MTLFunction> warmFunction = warm.functionNames.count ? [warm newFunctionWithName:warm.functionNames[0]] : nil;
    if (!warmFunction) return 3;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) return 4;
    limit.rlim_cur = MIN((rlim_t)256, limit.rlim_max);
    if (setrlimit(RLIMIT_NOFILE, &limit)) return 4;
    unsigned before = fd_count(limit.rlim_cur), peak = before;
    NSMutableArray *libraries = [NSMutableArray array], *functions = [NSMutableArray array];
    NSMutableArray *errors = [NSMutableArray array];
    BOOL url = argc == 5;
    for (unsigned i = 0; i < count; i++) @autoreleasepool {
        NSString *path = [output stringByAppendingPathComponent:[NSString stringWithFormat:@"%u.metallib", i]];
        error = nil;
        if (![fm copyItemAtPath:source toPath:path error:&error]) { [errors addObject:@"copy failed"]; break; }
        id<MTLLibrary> library = url ? [device newLibraryWithURL:[NSURL fileURLWithPath:path] error:&error]
                                    : totk_shader_library_load(device, path, &error);
        if (!library) { [errors addObject:@"library load failed"]; break; }
        id<MTLFunction> function = [library newFunctionWithName:warmFunction.name];
        if (!function) { [errors addObject:@"entry missing"]; break; }
        [libraries addObject:library]; [functions addObject:function];
        peak = MAX(peak, fd_count(limit.rlim_cur));
    }
    // Arrays remain strongly held during every sample and report construction.
    NSDictionary *report = @{@"status": errors.count ? @"failed" : @"pass", @"mode": url ? @"url-control" : @"production-memory-loader",
        @"requested": @(count), @"held_libraries": @(libraries.count), @"held_functions": @(functions.count),
        @"fd_soft_limit": @(limit.rlim_cur), @"fd_baseline": @(before), @"fd_peak": @(peak),
        @"fd_final": @(fd_count(limit.rlim_cur)), @"errors": errors};
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    puts([[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
    return errors.count ? 1 : 0;
} }
