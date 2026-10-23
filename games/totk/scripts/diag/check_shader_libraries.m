// Load each matching persistent Metal library and validate its reflected entry.
// No command queue, pipeline or GPU work is created. --source measures the
// supplemental source-compilation path, not gameplay or FPS.
// Compile from the source checkout, keeping the executable outside it:
// xcrun clang -fobjc-arc -O2 -Wall -Wextra -Wno-unused-function -Wno-deprecated-declarations \
//   -Igames/totk/third_party/d2r_gfx/gfx/nvn games/totk/scripts/diag/check_shader_libraries.m \
//   -framework Foundation -framework Metal -o /tmp/check_shader_libraries
// Usage: /tmp/check_shader_libraries <cache-dir> <user-libraries-root> [--source]
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "dmsh_chain.h"
#include "totk_shader_libraries.h"

static double milliseconds(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec * 1000.0 + value.tv_nsec / 1000000.0;
}

static double percentile(NSArray<NSNumber *> *sorted, double fraction) {
    if (!sorted.count) return 0;
    NSUInteger index = (NSUInteger)ceil(fraction * sorted.count) - 1;
    return sorted[MIN(index, sorted.count - 1)].doubleValue;
}

static NSDictionary *timing_summary(NSArray<NSNumber *> *times) {
    NSArray<NSNumber *> *sorted = [times sortedArrayUsingSelector:@selector(compare:)];
    double sum = 0;
    for (NSNumber *time in times) sum += time.doubleValue;
    return @{@"sum": @(sum), @"p50": @(percentile(sorted, 0.50)), @"p95": @(percentile(sorted, 0.95)),
             @"p99": @(percentile(sorted, 0.99)), @"max": sorted.lastObject ?: @0};
}

static void record_error(NSMutableArray *errors, NSString *hash, NSString *phase, NSError *error) {
    // Compiler diagnostics can embed source lines. Keep only a generic phase,
    // identity and error code; never publish the source or localizedDescription.
    NSMutableDictionary *row = [@{@"source_sha256": hash, @"phase": phase} mutableCopy];
    if (error) { row[@"error_domain"] = error.domain; row[@"error_code"] = @(error.code); }
    [errors addObject:row];
}

static void write_report(NSMutableDictionary *report, NSArray<NSNumber *> *times, double started) {
    report[@"total_wall_ms"] = @(milliseconds() - started);
    report[@"library_ms"] = timing_summary(times);
    NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingSortedKeys error:nil];
    fwrite(json.bytes, 1, json.length, stdout);
    fputc('\n', stdout);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        double started = milliseconds();
        BOOL sourceMode = argc == 4 && !strcmp(argv[3], "--source");
        NSMutableArray<NSNumber *> *times = [NSMutableArray array];
        NSMutableArray *errors = [NSMutableArray array], *entries = [NSMutableArray array];
        NSMutableDictionary *report = [@{@"schema": @1, @"mode": sourceMode ? @"source" : @"persisted",
            @"count": @0, @"failed": @0, @"passed": @0, @"errors": errors, @"libraries": entries,
            @"percentile_method": @"nearest-rank"} mutableCopy];
        if (argc < 3 || argc > 4 || (argc == 4 && !sourceMode)) {
            report[@"fatal_error"] = @"usage: check_shader_libraries <cache-dir> <user-libraries-root> [--source]";
            write_report(report, times, started); return 2;
        }
        NSString *cache = @(argv[1]);
        NSDictionary<NSString *, NSString *> *paths = totk_shader_libraries_read(@(argv[2]), @DMSH_CHAIN);
        if (!paths) {
            report[@"fatal_error"] = @"invalid user-library manifest";
            write_report(report, times, started); return 1;
        }
        NSError *scanError = nil;
        NSArray<NSString *> *files = [NSFileManager.defaultManager contentsOfDirectoryAtPath:cache error:&scanError];
        if (!files) {
            report[@"fatal_error"] = @"cannot read shader cache";
            report[@"error_code"] = @(scanError.code);
            write_report(report, times, started); return 1;
        }
        NSRegularExpression *names = [NSRegularExpression regularExpressionWithPattern:
            @"^[0-9a-f]{64}(?:-[0-9a-f]{12})?_b\\.metal$" options:0 error:nil];
        NSMutableDictionary<NSString *, NSDictionary *> *selected = [NSMutableDictionary dictionary];
        NSUInteger pairs = 0;
        for (NSString *name in [files sortedArrayUsingSelector:@selector(compare:)]) {
            if (![names numberOfMatchesInString:name options:0 range:NSMakeRange(0, name.length)]) continue;
            NSString *path = [cache stringByAppendingPathComponent:name];
            NSString *reflectionPath = [[path stringByDeletingPathExtension] stringByAppendingPathExtension:@"json"];
            if (![NSFileManager.defaultManager fileExistsAtPath:reflectionPath]) continue;
            NSData *data = [NSData dataWithContentsOfFile:path];
            if (!data) continue;
            NSString *hash = totk_shader_scope_hash(data);
            if (!paths[hash]) continue;
            pairs++;
            if (selected[hash]) continue;
            NSData *raw = [NSData dataWithContentsOfFile:reflectionPath];
            id reflection = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil] : nil;
            // Save only paths and reflection, not the full sources; load source
            // bytes afresh in the per-library autorelease pool when requested.
            selected[hash] = @{@"path": path, @"reflection": reflection ?: NSNull.null};
        }
        report[@"matched_pairs"] = @(pairs);
        report[@"manifest_libraries"] = @(paths.count);
        report[@"count"] = @(selected.count);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            report[@"fatal_error"] = @"no Metal device available";
            write_report(report, times, started); return 1;
        }
        MTLCompileOptions *options = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) options.mathMode = MTLMathModeSafe;
        else options.fastMathEnabled = NO;
        NSDictionary *kinds = @{@"vertex": @(MTLFunctionTypeVertex), @"fragment": @(MTLFunctionTypeFragment),
                                 @"kernel": @(MTLFunctionTypeKernel)};
        NSDictionary *stages = @{@"vs": @"vertex", @"ps": @"fragment", @"cs": @"kernel"};
        NSUInteger failed = 0;
        for (NSString *hash in [selected.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            @autoreleasepool {
                NSDictionary *row = selected[hash];
                id reflection = row[@"reflection"];
                double begin = milliseconds();
                BOOL ok = NO;
                if (![reflection isKindOfClass:NSDictionary.class] || ![reflection[@"chain"] isEqual:@DMSH_CHAIN] ||
                    ![reflection[@"key"] isEqual:[row[@"path"] lastPathComponent].stringByDeletingPathExtension] ||
                    ![reflection[@"entry"] isKindOfClass:NSString.class] || ![reflection[@"entry"] length] ||
                    ![reflection[@"msl_entry_kind"] isKindOfClass:NSString.class] ||
                    ![reflection[@"stage"] isKindOfClass:NSString.class] ||
                    ![stages[reflection[@"stage"]] isEqual:reflection[@"msl_entry_kind"]]) {
                    record_error(errors, hash, @"invalid reflection", nil);
                } else {
                    NSError *error = nil;
                    id<MTLLibrary> library;
                    if (sourceMode) {
                        NSData *data = [NSData dataWithContentsOfFile:row[@"path"] options:0 error:&error];
                        NSString *source = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : nil;
                        // Reject a cache write that raced with the scan instead
                        // of attributing a different source to this manifest hash.
                        if (!data || ![totk_shader_scope_hash(data) isEqual:hash] || !source) {
                            record_error(errors, hash, @"source changed or unreadable", error);
                            library = nil;
                        } else {
                            library = [device newLibraryWithSource:source options:options error:&error];
                            if (!library) record_error(errors, hash, @"source compilation", error);
                        }
                    } else {
                        library = totk_shader_library_load(device, paths[hash], &error);
                        if (!library) record_error(errors, hash, @"persisted library loading", error);
                    }
                    if (library) {
                        id<MTLFunction> function = [library newFunctionWithName:reflection[@"entry"]];
                        if (!function) record_error(errors, hash, @"reflected entry missing", nil);
                        else if (function.functionType != [kinds[reflection[@"msl_entry_kind"]] unsignedIntegerValue])
                            record_error(errors, hash, @"reflected function type mismatch", nil);
                        else ok = YES;
                    }
                }
                double elapsed = milliseconds() - begin;
                [times addObject:@(elapsed)];
                [entries addObject:@{@"source_sha256": hash, @"ms": @(elapsed), @"ok": @(ok)}];
                if (!ok) failed++;
            }
        }
        report[@"failed"] = @(failed);
        report[@"passed"] = @(selected.count - failed);
        write_report(report, times, started);
        return failed ? 1 : 0;
    }
}
