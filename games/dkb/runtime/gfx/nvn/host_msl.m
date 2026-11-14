#import "host_msl.h"
#include "host_msl_gen.h"
#include <os/lock.h>
#include <string.h>
#include <time.h>

#define HLOG(...) do { fprintf(stderr, "[dm] [msl] " __VA_ARGS__); fputc('\n', stderr); } while (0)

id<MTLLibrary> host_msl_lib(id<MTLDevice> dev, id<MTL4Compiler> comp, const char *name, NSError **e) {
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT; static NSMutableDictionary<NSString *, id<MTLLibrary>> *libs; static int said;
    if (!dev || !name) return nil;
    NSString *key = [NSString stringWithFormat:@"%p/%s", (__bridge void *)dev, name];
    os_unfair_lock_lock(&lk);
    if (!libs) libs = [NSMutableDictionary dictionary];
    id<MTLLibrary> lib = libs[key];
    if (!said++) HLOG("host shader libraries: %lu, compiled for %s by %s", sizeof k_host_msl / sizeof *k_host_msl, HOST_MSL_SDK, HOST_MSL_COMPILER);
    os_unfair_lock_unlock(&lk);
    if (lib) return lib;
    int i = 0, n = (int)(sizeof k_host_msl / sizeof *k_host_msl);
    while (i < n && strcmp(k_host_msl[i].name, name)) i++;
    if (i == n) { HLOG("[RED] no host shader library %s (dm/gfx/msl/%s.metal)", name, name); return nil; }
    NSError *le = nil;
    if (k_host_msl[i].lib_len) {
        dispatch_data_t d = dispatch_data_create(k_host_msl[i].lib, k_host_msl[i].lib_len, NULL, ^{});   // static bytes: nothing to free
        lib = [dev newLibraryWithData:d error:&le];
    }
    if (!lib) {   // the build had no Metal compiler, or this system does not take the library: compile its source (through the MTLDevice, also from MTL4Compiler)
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        NSString *src = @(k_host_msl[i].src);
        if (comp) { MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new]; ld.source = src; lib = [comp newLibraryWithDescriptor:ld error:e]; }
        else lib = [dev newLibraryWithSource:src options:nil error:e];
        HLOG("[RED] host shader library %s %s: compiled from its source in %.1f ms%s", name,
             k_host_msl[i].lib_len ? [NSString stringWithFormat:@"did not load (%@)", le.localizedDescription].UTF8String : "was not compiled when the host was built (no Metal toolchain)",
             (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6, lib ? "" : " and failed");
        if (!lib) return nil;
    }
    lib.label = @(name);
    os_unfair_lock_lock(&lk);
    id<MTLLibrary> had = libs[key]; if (had) lib = had; else libs[key] = lib;   // another thread made it meanwhile: one library per name
    os_unfair_lock_unlock(&lk);
    return lib;
}
