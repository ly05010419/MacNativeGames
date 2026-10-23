// Content-addressed, offline-built user libraries. Invalid or missing entries
// leave the runtime source compiler available; this is only a cache lookup.
#pragma once
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import "totk_shader_scope.h"
#import <Metal/Metal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static NSDictionary<NSString *, NSString *> *totk_shader_libraries_read(NSString *root, NSString *chain) {
    if (!root.length) return nil;
    NSData *data = [NSData dataWithContentsOfFile:[root stringByAppendingPathComponent:@"manifest.json"]];
    id m = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if (![m isKindOfClass:NSDictionary.class] || ![m[@"schema"] isEqual:@1] || ![m[@"chain"] isEqual:chain] ||
        ![m[@"options"] isKindOfClass:NSDictionary.class] || ![m[@"options"][@"math"] isEqual:@"safe"] ||
        ![m[@"options"][@"flags"] isEqual:@[@"-fmetal-math-mode=safe"]] ||
        ![m[@"options"][@"target"] isEqual:@"compiler-default"] ||
        ![m[@"compiler_identity"] isKindOfClass:NSDictionary.class] ||
        ![m[@"compiler_identity"][@"platform"] isEqual:@"darwin"] ||
        !totk_shader_scope_hex(m[@"directory"], 64) || ![m[@"libraries"] isKindOfClass:NSDictionary.class]) return nil;
    NSMutableDictionary<NSString *, NSString *> *paths = [NSMutableDictionary dictionary];
    NSString *directory = m[@"directory"];
    for (id digest in m[@"libraries"]) {
        if (!totk_shader_scope_hex(digest, 64)) return nil;
        NSString *relative = [NSString stringWithFormat:@"%@/%@.metallib", directory, digest];
        if (![m[@"libraries"][digest] isEqual:relative]) return nil;
        paths[digest] = [root stringByAppendingPathComponent:relative];
    }
    return [paths copy];
}

// The same mmap/close/newLibraryWithData lifetime used by lib_from_file on Mac.
// Keep the new user-cache path and its diagnostics on this descriptor-free API.
static inline id<MTLLibrary> totk_shader_library_load(id<MTLDevice> device, NSString *path, NSError **error) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    int problem = fd < 0 ? errno : 0;
    struct stat st;
    if (fd >= 0 && fstat(fd, &st)) problem = errno;
    else if (fd >= 0 && st.st_size <= 0) problem = EINVAL;
    if (problem) {
        if (fd >= 0) close(fd);
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:problem userInfo:nil];
        return nil;
    }
    size_t size = (size_t)st.st_size;
    void *bytes = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    problem = errno; close(fd);
    if (bytes == MAP_FAILED) {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:problem userInfo:nil];
        return nil;
    }
    dispatch_data_t data = dispatch_data_create(bytes, size, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(bytes, size); });
    return [device newLibraryWithData:data error:error];
}
#endif // Mac-only persistent libraries; no code or manifest reads on iOS.
