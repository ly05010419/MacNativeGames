// warm_metal_cache: compile the app's startup shaders once at packaging time, so the player's first start finds them in
// the system's Metal cache instead of compiling them in the background for about three minutes (M4, 8 threads; one
// thread took 14 minutes).
//
// Metal keeps the compiled libraries of programs without an app bundle (the game binary is one: the launcher starts it)
// in one store, keyed by the shader source text, the compile options and the length of the path of the folder the
// program runs from -- only its length (2026-10-18): a program in a folder whose path is one character longer finds
// nothing, while any program in a folder with a path as long (same folder, other name or code-signing identifier,
// another volume, another depth) finds everything; the working directory and the way the program was started do not
// matter. So build_app.py copies this tool next to the game binary (Contents/Resources/totk/build), runs it there and
// removes it again. The game compiles each <variant>_b.metal of its shader cache with exactly these options
// (nvn_mtl.m totk_precise_opts / totk_msl_library), and the startup seed is copied into that cache byte for byte
// (totk_shader_seed.h), so compiling the seed's files here gives the game cache hits (0.03-0.5 ms instead of 50-130 ms).
// The store is shared by all such programs, evicts heavily when one of them compiles many new libraries, and loses what
// a program compiled while another program that opened it first was running as soon as that program quits. So this runs
// last, only while no other program has the store open (build_app.py metal_cache_users), and is checked with --verify
// in a new process.
//
// usage: warm_metal_cache <folder with *_b.metal> [threads]   compile them all; prints one JSON line
//        warm_metal_cache <folder> --verify <count>           time <count> of them spread over the folder; prints one JSON line
// build: xcrun clang -fobjc-arc -O2 tools/warm_metal_cache.m -framework Metal -framework Foundation -o build/warm-metal-cache
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

// The game's options (nvn_mtl.m totk_precise_opts): exactly rounded IEEE float arithmetic. Anything else is another
// cache entry. (The game also has a fastMathEnabled = NO branch for macOS before 15; the app needs macOS 26.)
static MTLCompileOptions *game_options(void) {
    MTLCompileOptions *o = [MTLCompileOptions new];
    o.mathMode = MTLMathModeSafe;
    return o;
}

// The game's warm-up skips these (nvn_mtl.m warm_shader_load): Metal has no double, and spirv-cross marks what it could
// not translate.
static int skipped_source(NSString *src) {
    return [src containsString:@"double "] || [src containsString:@"unsupported_GLSLstd450"];
}

static NSArray<NSString *> *shader_sources(NSString *dir) {
    NSError *e = nil;
    NSArray<NSString *> *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:&e];
    if (!names) { fprintf(stderr, "warm_metal_cache: cannot list %s: %s\n", dir.UTF8String, e.localizedDescription.UTF8String); exit(2); }
    NSMutableArray<NSString *> *out = [NSMutableArray array];
    for (NSString *n in [names sortedArrayUsingSelector:@selector(compare:)])
        if ([n hasSuffix:@"_b.metal"]) [out addObject:[dir stringByAppendingPathComponent:n]];
    return out;
}

// One library as the game compiles it: the file's UTF-8 text as is. Returns the compile time in ms, -1 when skipped,
// -2 when it does not compile. A compile cut off by the system's compiler service being stopped is retried, as in the
// game (totk_msl_library).
static double compile_one(id<MTLDevice> dev, MTLCompileOptions *opts, NSString *path, NSString **why) {
    NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!src) { if (why) *why = @"cannot read the file"; return -2; }
    if (skipped_source(src)) return -1;
    for (int attempt = 1;; attempt++) {
        NSError *e = nil; CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opts error:&e];
        double ms = (CFAbsoluteTimeGetCurrent() - t0) * 1e3;
        if (lib) return ms;
        BOOL interrupted = [e.localizedDescription containsString:@"XPC_ERROR_CONNECTION_INTERRUPTED"];
        if (!interrupted || attempt == 4) { if (why) *why = e.localizedDescription ?: @"no error text"; return -2; }
        usleep((useconds_t)attempt * 100000);
    }
}

static int compile_all(id<MTLDevice> dev, NSString *dir, int threads) {
    NSArray<NSString *> *files = shader_sources(dir);
    MTLCompileOptions *opts = game_options();
    __block _Atomic long next = 0, compiled = 0, skipped = 0, failed = 0;
    __block _Atomic long long busy_us = 0;
    NSMutableArray<NSString *> *problems = [NSMutableArray array];
    CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
    dispatch_group_t g = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    for (int t = 0; t < threads; t++) dispatch_group_async(g, q, ^{
        for (long i; (i = atomic_fetch_add(&next, 1)) < (long)files.count;) @autoreleasepool {
            NSString *why = nil; double ms = compile_one(dev, opts, files[(NSUInteger)i], &why);
            if (ms == -1) atomic_fetch_add(&skipped, 1);
            else if (ms == -2) {
                atomic_fetch_add(&failed, 1);
                @synchronized (problems) { if (problems.count < 5) [problems addObject:[NSString stringWithFormat:@"%@: %@", files[(NSUInteger)i].lastPathComponent, why]]; }
            } else { atomic_fetch_add(&compiled, 1); atomic_fetch_add(&busy_us, (long long)(ms * 1e3)); }
            long done = atomic_load(&compiled) + atomic_load(&skipped) + atomic_load(&failed);
            if (done % 1000 == 0) fprintf(stderr, "warm_metal_cache: %ld / %lu after %.0f s\n", done, (unsigned long)files.count, CFAbsoluteTimeGetCurrent() - t0);
        }
    });
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    for (NSString *p in problems) fprintf(stderr, "warm_metal_cache: does not compile: %s\n", p.UTF8String);
    printf("{\"files\":%lu,\"compiled\":%ld,\"skipped\":%ld,\"failed\":%ld,\"threads\":%d,\"seconds\":%.1f,\"compile_seconds_summed\":%.1f}\n",
           (unsigned long)files.count, atomic_load(&compiled), atomic_load(&skipped), atomic_load(&failed), threads,
           CFAbsoluteTimeGetCurrent() - t0, atomic_load(&busy_us) / 1e6);
    return 0;
}

static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }

// Hits take 0.1-0.5 ms, a library Metal has not seen 20-90 ms (M4); 5 ms separates them.
enum { HIT_MS = 5 };
static int verify(id<MTLDevice> dev, NSString *dir, int count) {
    NSArray<NSString *> *files = shader_sources(dir);
    MTLCompileOptions *opts = game_options();
    // The first request of a process also starts the connection to the compiler service: not part of the sample.
    [dev newLibraryWithSource:@"#include <metal_stdlib>\nkernel void warm_metal_cache_hello(uint i [[thread_position_in_grid]]) {}\n" options:opts error:nil];
    double *ms = calloc((size_t)count, sizeof *ms); int n = 0, hits = 0;
    for (int k = 0; k < count && files.count; k++) @autoreleasepool {
        NSUInteger i = (NSUInteger)((double)k * (double)files.count / count);
        double t = compile_one(dev, opts, files[i], NULL);
        if (t < 0) continue;
        ms[n++] = t; if (t < HIT_MS) hits++;
    }
    qsort(ms, (size_t)n, sizeof *ms, cmp_double);
    printf("{\"sampled\":%d,\"hits\":%d,\"median_ms\":%.2f,\"max_ms\":%.2f}\n", n, hits, n ? ms[n / 2] : 0.0, n ? ms[n - 1] : 0.0);
    free(ms);
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: warm_metal_cache <folder> [threads] | <folder> --verify <count>\n"); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "warm_metal_cache: no Metal device\n"); return 2; }
        NSString *dir = @(argv[1]);
        if (argc >= 4 && !strcmp(argv[2], "--verify")) return verify(dev, dir, atoi(argv[3]) > 0 ? atoi(argv[3]) : 64);
        // As the game's warm-up (nvn_mtl.m warm_threads): the core count minus 2, at most 8.
        int threads = argc >= 3 ? atoi(argv[2]) : (int)NSProcessInfo.processInfo.activeProcessorCount - 2;
        if (threads > 8 && argc < 3) threads = 8;
        if (threads < 1) threads = 1;
        return compile_all(dev, dir, threads);
    }
}
