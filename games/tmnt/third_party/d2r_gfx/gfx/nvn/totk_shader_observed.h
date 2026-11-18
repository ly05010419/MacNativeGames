// Persist raw shader keys actually supplied by this game, including cache hits.
// The bundled inventory is incomplete for some dynamically loaded game shaders.
#pragma once
#import "totk_shader_scope.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

// The small mutable state is private to the translator. Access is synchronized,
// and snapshots are immutable so the warm-up can consume them on another thread.
// scope: totk_shader_scope_metadata() of this app's seed (parsed once by the caller).
static NSMutableDictionary *totk_shader_observed_open(NSDictionary *scope, NSString *cacheDir, NSString *chain) {
    if (!scope || ![cacheDir isKindOfClass:NSString.class] || !cacheDir.length) return nil;
    NSDictionary *identity = @{@"schema": @1, @"chain": chain, @"version": scope[@"version"],
                               @"source_romfs_sha256": scope[@"source_romfs_sha256"]};
    NSString *path = [[cacheDir stringByAppendingPathComponent:@"observed"]
        stringByAppendingPathComponent:[scope[@"source_romfs_sha256"] stringByAppendingPathExtension:@"keys"]];
    NSSet<NSString *> *staticKeys = [NSSet setWithArray:scope[@"keys"]];
    NSMutableSet<NSString *> *keys = [NSMutableSet set];
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    NSArray<NSString *> *lines = [text componentsSeparatedByString:@"\n"];
    NSData *header = lines.count ? [lines[0] dataUsingEncoding:NSUTF8StringEncoding] : nil;
    id previous = header ? [NSJSONSerialization JSONObjectWithData:header options:0 error:nil] : nil;
    BOOL valid = [previous isKindOfClass:NSDictionary.class] && [previous isEqual:identity];
    if (valid) for (NSUInteger i = 1; i < lines.count; i++) {
        NSString *key = lines[i];
        if (totk_shader_scope_hex(key, 64) && ![staticKeys containsObject:key]) [keys addObject:key];
    }
    return [@{@"identity": identity, @"path": path, @"scope": staticKeys,
               @"keys": keys, @"valid": @(valid)} mutableCopy];
}

static NSSet<NSString *> *totk_shader_observed_keys(NSMutableDictionary *state) {
    if (!state) return [NSSet set];
    @synchronized (state) { return [state[@"keys"] copy]; }
}

// A key gets at most one append attempt per process. Persistence is best effort:
// a full/read-only cache must not block drawing or repeatedly retry each frame.
// The key is live at once; the file is written on a serial queue (the submit thread spent milliseconds per new shader
// opening and appending, 2026-10-22), and whatever is still queued is written before the process exits.
static dispatch_queue_t totk_shader_observed_queue;
static void totk_shader_observed_flush(void) { if (totk_shader_observed_queue) dispatch_sync(totk_shader_observed_queue, ^{}); }
static BOOL totk_shader_observed_note(NSMutableDictionary *state, NSString *key) {
    if (!state || !totk_shader_scope_hex(key, 64)) return NO;
    NSString *path; NSData *line;
    @synchronized (state) {
        NSMutableSet<NSString *> *keys = state[@"keys"];
        if ([state[@"scope"] containsObject:key] || [keys containsObject:key]) return NO;
        [keys addObject:key];
        path = state[@"path"];
        line = [[key stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding];
    }
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        totk_shader_observed_queue = dispatch_queue_create("totk.shader.observed", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
        atexit(totk_shader_observed_flush);
    });
    dispatch_async(totk_shader_observed_queue, ^{
        BOOL valid; NSDictionary *identity;
        @synchronized (state) { valid = [state[@"valid"] boolValue]; identity = state[@"identity"]; }
        if (!valid) {
            NSFileManager *fm = NSFileManager.defaultManager;
            if (![fm createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]) return;
            NSData *header = [NSJSONSerialization dataWithJSONObject:identity options:NSJSONWritingSortedKeys error:nil];
            NSMutableData *data = [header mutableCopy]; [data appendBytes:"\n" length:1]; [data appendData:line];
            if ([data writeToFile:path options:NSDataWritingAtomic error:nil]) { @synchronized (state) { state[@"valid"] = @YES; } }
            return;
        }
        int fd = open(path.fileSystemRepresentation, O_WRONLY | O_APPEND | O_CLOEXEC);
        if (fd >= 0) {
            const uint8_t *bytes = line.bytes; NSUInteger left = line.length;
            while (left) {
                ssize_t n = write(fd, bytes, left);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) break;
                bytes += n; left -= (NSUInteger)n;
            }
            close(fd);
        }
    });
    return YES;
}
