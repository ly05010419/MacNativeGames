// Metal 4 pipelines (pso_build): one base per pipeline with its colour state unspecialized, made into pipelines by specialization, kept in the pipeline archive.
// Checks: pipelines that differ only in colour state share one base; a specialized pipeline draws the same pixels as one built through the MTLDevice; a flushed archive
// file serves the base again without compiling; a flush of bases without a vertex layout is a p<seq> file, where a base with a layout is not looked up; a function with no
// Metal 4 descriptor keeps the MTLDevice path; pso_key_norm drops what no pipeline depends on; compaction merges the files the prewarm took bases from into one despite a
// mid-prewarm flush, and a compaction that did not finish is not tried again on the same system build; depth-only draws write depth; concurrent colour variants compile
// one base; archive flushes preserve unrelated compiler state
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
// Keep the host's cache observations/deletions in a directory owned by this test.
// Metal itself still uses its real compiler cache; only this translation unit's
// confstr calls see the fixture parent, including when testing the former cleanup.
static char archive_cache_root[] = "/tmp/t_pso_archive_cache.XXXXXX";
static size_t archive_confstr(int name, char *buffer, size_t length) {
    if (name != _CS_DARWIN_USER_CACHE_DIR) return confstr(name, buffer, length);
    size_t size = strlen(archive_cache_root) + 1;
    if (length && buffer) { size_t n = size < length ? size : length; memcpy(buffer, archive_cache_root, n); buffer[n - 1] = 0; }
    return size;
}
#define confstr archive_confstr
#include "../nvn/nvn_mtl.m"
static void archive_cache_cleanup(void) {
    [NSFileManager.defaultManager removeItemAtPath:@(archive_cache_root) error:nil];
}
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static id<MTLLibrary> g_lib, g_lib2;   // g_lib2: the same source again, for functions with no Metal 4 descriptor (a library hands out one function object per name)
static id<MTLCommandQueue> g_q;
static id<MTLFunction> test_fn(id<MTLLibrary> lib, NSString *name, int registered) {
    id<MTLFunction> f = [lib newFunctionWithName:name];
    MTLFunctionConstantValues *cv = f.functionConstantsDictionary.count ? [MTLFunctionConstantValues new] : nil;
    if (cv) f = [lib newFunctionWithName:name constantValues:cv error:nil];
    return registered ? fn4_note(f, lib, name, cv) : f;
}
// Hold the first base build while other callers request colour variants and an unrelated base.
@interface ArchiveGateCompiler : NSObject {
@public id<MTL4Compiler> compiler; dispatch_semaphore_t entered, release; _Atomic int calls, lookup_calls; int fail;
}
@end
@implementation ArchiveGateCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    if (opts.lookupArchives.count) atomic_fetch_add(&lookup_calls, 1);
    int call = atomic_fetch_add(&calls, 1);
    if (!call) { dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER); }
    if (fail) { if (error) *error = [NSError errorWithDomain:@"test" code:1 userInfo:nil]; return nil; }
    return [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
@end
// Notes whether the compaction marker exists while the serializer writes (issue #342: a flush that does not return leaves it)
@interface MarkerSerializer : NSObject { @public id<MTL4PipelineDataSetSerializer> ser; int calls, marked; } @end
@implementation MarkerSerializer
- (id)forwardingTargetForSelector:(SEL)selector { return ser; }
- (BOOL)serializeAsArchiveAndFlushToURL:(NSURL *)url error:(NSError **)error {
    calls++; marked += [[NSString stringWithContentsOfFile:arch_compact_marker() encoding:NSUTF8StringEncoding error:nil] isEqualToString:arch_compact_stamp()];
    return [ser serializeAsArchiveAndFlushToURL:url error:error];
}
@end
// Model another compiler creating state before the real compiler call returns.
// The UUID fixture belongs exclusively to this test; no discovered folder is removed.
@interface ArchiveForeignCompiler : NSObject {
@public id<MTL4Compiler> compiler; NSString *fixture; NSError *fixture_error;
    unsigned calls; BOOL created, marker_created;
}
@end
@implementation ArchiveForeignCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    calls++;
    id<MTLRenderPipelineState> ps = [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
    if (ps && !created) {
        if (mkdir(fixture.fileSystemRepresentation, 0700)) fixture_error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        else {
            created = YES;
            NSString *marker = [fixture stringByAppendingPathComponent:@"PersistentState"];
            int fd = open(marker.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0) fixture_error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
            else { close(fd); marker_created = YES; }
        }
    }
    return ps;
}
@end
// a full-screen triangle; colour 0 = (0.5, 0.25, 1, 0.5), colour 1 = (0.2, 0.4, 0.6, 0.8); attribute 0 (a float4) scales nothing, so a vertex descriptor is part of the pipeline
static MTLRenderPipelineDescriptor *desc(MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1, int registered) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLFunction> v = test_fn(g_lib2, @"vs", 0), f = test_fn(g_lib2, @"fs", 0);
    static id<MTLFunction> rv, rf;   // the registered ones are made once, as shader_load makes a shader's functions once
    if (registered) { if (!rv) { rv = test_fn(g_lib, @"vs", 1); rf = test_fn(g_lib, @"fs", 1); } v = rv; f = rf; }
    d.vertexFunction = v; d.fragmentFunction = f;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 16; vd.layouts[16].stride = 16; vd.layouts[16].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[16].stepRate = 0;
    d.vertexDescriptor = vd;
    d.colorAttachments[0].pixelFormat = f0;
    if (blend) { d.colorAttachments[0].blendingEnabled = YES; d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                 d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha; }
    d.colorAttachments[1].pixelFormat = f1; d.colorAttachments[1].writeMask = m1;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;   // a Metal 4 pipeline names no depth format; drawn into a pass with this depth attachment
    return d;
}
// draws with ps into fresh targets cleared to (1, 0, 0, 1) and returns texel (1, 1) of both as floats
static void draw_with(id<MTLRenderPipelineState> ps, MTLPixelFormat f0, MTLPixelFormat f1, float out[2][4]) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; id<MTLTexture> t[2];
    MTLPixelFormat fm[2] = { f0, f1 };
    for (int i = 0; i < 2; i++) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fm[i] width:4 height:4 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        t[i] = [g_dev newTextureWithDescriptor:td];
        rp.colorAttachments[i].texture = t[i]; rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].clearColor = MTLClearColorMake(1, 0, 0, 1); rp.colorAttachments[i].storeAction = MTLStoreActionStore;
    }
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:4 height:4 mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
    rp.depthAttachment.texture = [g_dev newTextureWithDescriptor:dd]; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionDontCare;
    static id<MTLBuffer> one; if (!one) { float v[4] = { 1, 1, 1, 1 }; one = [g_dev newBufferWithBytes:v length:16 options:MTLResourceStorageModeShared]; }
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:ps]; [e setVertexBuffer:one offset:0 atIndex:16]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    for (int i = 0; i < 2; i++) {
        if (fm[i] == MTLPixelFormatRGBA8Unorm) { uint8_t p[4]; [t[i] getBytes:p bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c] / 255.0f; }
        else { __fp16 p[4]; [t[i] getBytes:p bytesPerRow:32 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0]; for (int c = 0; c < 4; c++) out[i][c] = p[c]; }
    }
}
static void same_pixels(const char *name, MTLPixelFormat f0, int blend, MTLPixelFormat f1, MTLColorWriteMask m1) {
    NSError *e = nil;
    id<MTLRenderPipelineState> a = pso_build(desc(f0, blend, f1, m1, 1), &e), b = [g_dev newRenderPipelineStateWithDescriptor:desc(f0, blend, f1, m1, 0) error:&e];
    checks++; if (!a || !b) { fprintf(stderr, "%s: no pipeline (%s)\n", name, e.localizedDescription.UTF8String); failed++; return; }
    float pa[2][4], pb[2][4]; draw_with(a, f0, f1, pa); draw_with(b, f0, f1, pb);
    checks++; if (memcmp(pa, pb, sizeof pa)) { fprintf(stderr, "%s: the specialized pipeline drew (%g %g %g %g | %g %g %g %g), the MTLDevice one (%g %g %g %g | %g %g %g %g)\n", name,
        pa[0][0], pa[0][1], pa[0][2], pa[0][3], pa[1][0], pa[1][1], pa[1][2], pa[1][3], pb[0][0], pb[0][1], pb[0][2], pb[0][3], pb[1][0], pb[1][1], pb[1][2], pb[1][3]); failed++; }
}
static void depth_only(void) {
    MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatInvalid, 0, MTLPixelFormatInvalid, MTLColorWriteMaskNone, 1);
    d.fragmentFunction = nil; NSError *err = nil; uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> ps = pso_build(d, &err);
    EXPECT("depth only: pipeline", 1, ps != nil);
    EXPECT("depth only: legacy path", legacy + 1, g_pb_legacy);
    if (!ps) return;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new]; rp.depthAttachment.texture = t;
    rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new]; ds.depthWriteEnabled = YES; ds.depthCompareFunction = MTLCompareFunctionAlways;
    float v[4] = {1, 1, 1, 1}; id<MTLBuffer> vb = [g_dev newBufferWithBytes:v length:sizeof v options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setRenderPipelineState:ps]; [enc setDepthStencilState:[g_dev newDepthStencilStateWithDescriptor:ds]];
    [enc setVertexBuffer:vb offset:0 atIndex:16]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 4, 1)
                toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:1024]; [blit endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("depth only: command completed", MTLCommandBufferStatusCompleted, cb.status);
    EXPECT("depth only: rasterization wrote zero depth", 1, ((float *)out.contents)[65] == 0.0f);
}
static void concurrent_bases(int fail) {
    nvn_hm_clear(&g_pbase, hm_rel);
    ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->fail = fail;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0); ep->comp = (id<MTL4Compiler>)gate;
    NSMutableArray<MTLRenderPipelineDescriptor *> *ds = [NSMutableArray array];
    for (int i = 0; i < 8; i++) {
        MTLRenderPipelineDescriptor *d = desc(MTLPixelFormatRGBA8Unorm, i & 1, MTLPixelFormatRGBA8Unorm, i, 1);
        d.vertexDescriptor.layouts[16].stride = 512; [ds addObject:d];
    }
    dispatch_group_t group = dispatch_group_create(); __block int good = 0; NSObject *lock = [NSObject new];
    for (MTLRenderPipelineDescriptor *d in ds) dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool { NSError *err = nil; id ps = pso_build(d, &err);
            @synchronized(lock) { good += fail ? !ps && [err.domain isEqualToString:@"test"] : ps != nil; }
        }
    });
    EXPECT("concurrent: first compiler entered", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    // An unrelated key must complete while the first remains blocked.
    MTLRenderPipelineDescriptor *other = [ds[0] copy]; other.vertexDescriptor = [other.vertexDescriptor copy]; other.vertexDescriptor.layouts[16].stride = 1024;
    dispatch_group_t separate = dispatch_group_create();
    dispatch_group_async(separate, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { pso_build(other, NULL); } });
    EXPECT("concurrent: unrelated base progresses", 0, dispatch_group_wait(separate, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER); dispatch_group_wait(separate, DISPATCH_TIME_FOREVER);
    EXPECT("concurrent: all callers receive result", 8, good);
    if (!fail) EXPECT("concurrent: one compile per base", 2, gate->calls);
    EXPECT("concurrent: no unfinished builds", 0, g_pbase_pending.n);
    ep->comp = gate->compiler;
    if (fail) { NSError *err = nil; EXPECT("concurrent: failed base can retry", 1, pso_build(ds[0], &err) != nil); }
}
static void capture_preserves_foreign_folder(void) {
    arch_flush_wait();   // Finish earlier captures: the captures below go into a new flush period
    NSError *error = nil;
    MTLRenderPipelineDescriptor *prime = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    prime.vertexDescriptor.layouts[16].stride = 1536;
    // Establish the period's working folder with a capture of its own compiler
    EXPECT("foreign capture: prime real compiler", 1,
           [g_ep->comp newRenderPipelineStateWithDescriptor:pso_desc4(prime, 1) compilerTaskOptions:nil error:&error] != nil);
    char root[PATH_MAX];
    if (!confstr(_CS_DARWIN_USER_CACHE_DIR, root, sizeof root)) { EXPECT("foreign capture: cache path available", 1, 0); return; }
    ArchEpoch *ep = g_ep; ArchiveForeignCompiler *proxy = [ArchiveForeignCompiler new]; proxy->compiler = ep->comp;
    NSString *cache = [@(root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
    proxy->fixture = [cache stringByAppendingPathComponent:[@"gpuarchiver-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    MTLRenderPipelineDescriptor *fresh = [prime copy]; fresh.vertexDescriptor = [prime.vertexDescriptor copy];
    fresh.vertexDescriptor.layouts[16].stride = 2048;
    ep->comp = (id<MTL4Compiler>)proxy;   // the current flush period's compiler, which the captures go through
    EXPECT("foreign capture: real pipeline", 1, pso_build(fresh, &error) != nil);
    ep->comp = proxy->compiler;
    EXPECT("foreign capture: one real compiler call", 1, proxy->calls);
    EXPECT("foreign capture: fixture created", 1, proxy->marker_created);
    if (proxy->fixture_error) fprintf(stderr, "foreign capture: fixture creation failed: %s\n", proxy->fixture_error.localizedDescription.UTF8String);
    arch_flush_wait();
    EXPECT("foreign capture: PersistentState survives flush", 1,
           [NSFileManager.defaultManager fileExistsAtPath:[proxy->fixture stringByAppendingPathComponent:@"PersistentState"]]);
    if (proxy->created && [NSFileManager.defaultManager fileExistsAtPath:proxy->fixture])
        EXPECT("foreign capture: test-owned fixture removed", 1, [NSFileManager.defaultManager removeItemAtPath:proxy->fixture error:nil]);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (!mkdtemp(archive_cache_root)) abort();
    atexit(archive_cache_cleanup);
    if (![NSFileManager.defaultManager createDirectoryAtPath:[@(archive_cache_root) stringByAppendingPathComponent:@"com.apple.gpuarchiver"]
                               withIntermediateDirectories:NO attributes:nil error:nil]) abort();
    char dir[] = "/tmp/t_pso_archive.XXXXXX"; if (!mkdtemp(dir)) abort();
    setenv("DM_PSO_ARCHIVE", argc == 3 ? argv[2] : dir, 1);
    mtl_init(); g_q = [g_dev newCommandQueue]; NSError *err = nil;
    NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\nstruct I { float4 a [[attribute(0)]]; };\n"
        "vertex V vs(I in [[stage_in]], uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, in.a.w); return o; }\n"
        "struct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; };\nfragment O fs() { return { float4(0.5, 0.25, 1.0, 0.5), float4(0.2, 0.4, 0.6, 0.8) }; }\n"
        "fragment O fs2() { return { float4(1), float4(0) }; }\n"
        "vertex V vs0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n";
    if (getenv("DM_TEST_ARCHIVE_CONSTANTS")) src = [src stringByReplacingOccurrencesOfString:@"fragment O fs() { return" withString:@"constant bool enabled [[function_constant(0)]]; fragment O fs() { if (is_function_constant_defined(enabled) && enabled) discard_fragment(); return"];
    if (argc == 3) {
        NSString *metal = [@(argv[2]) stringByAppendingPathComponent:@"fixture.metal"], *library = [metal stringByAppendingString:@"lib"];
        if (!strcmp(argv[1], "--compact-seed")) {
            [src writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil];
            NSTask *task = [NSTask new]; task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
            task.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", library];
            if (![task launchAndReturnError:&err]) return 1;
            [task waitUntilExit]; if (task.terminationStatus) return 1;
        }
        g_lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:library] error:&err];
    } else g_lib = [g_dev newLibraryWithSource:src options:nil error:&err];
    if (!g_lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    g_lib2 = [g_dev newLibraryWithSource:[src stringByAppendingString:@"// another library: the same source would come back as the same library\n"] options:nil error:&err];
    // A fresh process is essential: a compiler that built the source archives still has their online binaries.
    if (argc == 3) {
        int seed = !strcmp(argv[1], "--compact-seed");
        pso4_init();
        EXPECT("restart: opened run archives", seed ? 0 : !strcmp(argv[1], "--compact-replay") ? PSO_ARCH_COMPACT + 1 : 1, g_arch4.count);
        atomic_store(&g_warm_running, !seed);
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2);
            [descs addObject:cd];
            if (seed) { EXPECT("restart: pipeline", 1, pso_build(cd, &err) != nil); arch_flush_wait(); }
        }
        if (!seed) {
            int fresh = getenv("DM_TEST_ARCHIVE_MIXED") != NULL, compacting = g_compact_files != nil;
            int outcomes[PSO_ARCH_COMPACT + 1] = {0}; int *results = outcomes;
            dispatch_apply(descs.count, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
                @autoreleasepool { results[i] = pso_build(descs[i], NULL) != nil; }
            });
            for (int i = 0; i <= PSO_ARCH_COMPACT; i++) EXPECT("restart: pipeline", 1, outcomes[i]);
            if (fresh) {
                MTLRenderPipelineDescriptor *extra = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
                extra.fragmentFunction = test_fn(g_lib, @"fs2", 1);
                extra.vertexDescriptor.layouts[16].stride = 4096;
                EXPECT("restart: fresh pipeline mixed with recaptures", 1, pso_build(extra, &err) != nil);
            }
            EXPECT("restart: every base from disk", PSO_ARCH_COMPACT + 1 + (fresh && !compacting), g_pb_archived);
            EXPECT("restart: compiled bases", fresh && compacting, g_pb_built);
            dispatch_sync(g_arch_q, ^{ arch_flush(); });
            if (g_compact_files) EXPECT("restart: timed flush keeps recaptures pending", PSO_ARCH_COMPACT + 1 + fresh, g_arch_new);
            atomic_store(&g_warm_running, 0);
            arch_flush_wait();
        }
        if (seed && getenv("DM_TEST_ARCHIVE_MIXED")) [arch_os_build() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
        printf("t_pso_archive %s: %d checks, %d failed\n", argv[1], checks, failed);
        return failed != 0;
    }
    // pipelines differing only in colour state share one base
    same_pixels("RGBA8 blended, RGBA16F all channels", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("first pipeline: its base compiled", 1, g_pb_built);
    same_pixels("RGBA16F, RGBA8 red and green", MTLPixelFormatRGBA16Float, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskRed | MTLColorWriteMaskGreen);
    same_pixels("RGBA8, RGBA8 no channel", MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskNone);
    EXPECT("other colour states: the same base", 1, g_pb_built);
    EXPECT("other colour states: base reused", 2, g_pb_reused);
    EXPECT("one specialization per pipeline", 3, g_pb_spec);
    // the archive: flushed to a file, a later start takes the base from it without compiling
    EXPECT("one base waits for the archive", 1, g_arch_new);
    ArchEpoch *ep0 = g_ep;
    arch_flush_wait();
    NSString *file = [@(dir) stringByAppendingPathComponent:@"00000.mtl4a"];
    EXPECT("archive file written", 1, [NSFileManager.defaultManager fileExistsAtPath:file]);
    EXPECT("the flush began a new period", 1, g_ep != ep0);
    EXPECT("nothing left to write", 0, g_arch_new);
    nvn_hm_clear(&g_pbase, hm_rel);   // as a new start: no base in memory, the archive opened
    g_arch4 = @[[g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err]];
    uint64_t built = g_pb_built;
    same_pixels("from the archive: RGBA8 blended, RGBA16F", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("base from the archive", 1, g_pb_archived);
    EXPECT("no base compiled", built, g_pb_built);
    // a flush whose bases all read their vertices themselves (no vertex layout) is a p<seq> file; a base with a layout is never looked up in one
    MTLRenderPipelineDescriptor *nd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
    nd.vertexFunction = fn4_note([g_lib newFunctionWithName:@"vs0"], g_lib, @"vs0", nil); nd.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    EXPECT("no layout: a pipeline", 1, pso_build(nd, &err) != nil);
    EXPECT("no layout: its base compiled", built + 1, g_pb_built);
    arch_flush_wait();
    NSString *pfile = [@(dir) stringByAppendingPathComponent:@"p00001.mtl4a"];
    EXPECT("no layout: written to a p file", 1, [NSFileManager.defaultManager fileExistsAtPath:pfile]);
    nvn_hm_clear(&g_pbase, hm_rel);
    id<MTL4Archive> pa = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:pfile] error:&err], va = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:file] error:&err];
    g_arch4 = @[]; g_arch4p = @[pa, va];   // the layout base's file among the p files: not looked in
    uint64_t arch0 = g_pb_archived; built = g_pb_built;
    EXPECT("no layout again: a pipeline", 1, pso_build(nd, &err) != nil);
    EXPECT("no layout again: base from the p file", arch0 + 1, g_pb_archived);
    same_pixels("layout base with only p files open", MTLPixelFormatRGBA8Unorm, 1, MTLPixelFormatRGBA16Float, MTLColorWriteMaskAll);
    EXPECT("layout base: not taken from a p file, compiled", built + 1, g_pb_built);
    EXPECT("layout base: one waits for a plain file", 1, g_arch_new_vd);
    // compaction: with more than PSO_ARCH_COMPACT files, the bases the prewarm takes from them are captured again; after the flush that follows, one file holds them and the old ones are gone
    {
        char cdir[] = "/tmp/t_pso_compact.XXXXXX"; if (!mkdtemp(cdir)) abort();
        NSString *saved = g_arch_dir; g_arch_dir = @(cdir); nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = @[]; g_arch4p = @[];
        NSMutableArray<MTLRenderPipelineDescriptor *> *descs = [NSMutableArray array];
        for (int i = 0; i <= PSO_ARCH_COMPACT; i++) {   // one base per file: the stride of the layout tells them apart
            MTLRenderPipelineDescriptor *cd = desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 1);
            cd.vertexDescriptor.layouts[16].stride = 16 * (i + 2); [descs addObject:cd];
            if (!pso_build(cd, &err)) { fprintf(stderr, "compaction: no pipeline %d\n", i); failed++; }
            arch_flush_wait();
        }
        NSArray<NSString *> *names = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:@(cdir) error:nil] sortedArrayUsingSelector:@selector(compare:)];
        EXPECT("compaction: one file per flush", PSO_ARCH_COMPACT + 1, names.count);
        NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array]; NSMutableArray<NSString *> *paths = [NSMutableArray array];
        for (NSString *n in names.reverseObjectEnumerator) { NSString *f = [@(cdir) stringByAppendingPathComponent:n]; [paths addObject:f]; [open addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:f] error:&err]]; }
        nvn_hm_clear(&g_pbase, hm_rel); g_arch4 = open;   // as a new start with that many files
        g_compact_enabled = 1; g_compact_files = paths; g_compacted = 0;
        uint64_t arch0 = g_pb_archived, built0 = g_pb_built;
        ArchEpoch *ep = g_ep; ArchiveGateCompiler *gate = [ArchiveGateCompiler new]; gate->compiler = ep->comp; gate->calls = 1; ep->comp = (id<MTL4Compiler>)gate;
        atomic_store(&g_warm_running, 1);
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        EXPECT("compaction: captures original shader functions, not archive imports", 0, gate->lookup_calls);
        ep->comp = gate->compiler;
        dispatch_sync(g_arch_q, ^{ arch_flush(); });   // the timer fires while prewarm still has other work
        EXPECT("compaction: timed flush keeps recaptured bases pending", descs.count, g_arch_new);
        EXPECT("compaction: timed flush adds no partial file", PSO_ARCH_COMPACT + 1, [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(cdir) error:nil].count);
        atomic_store(&g_warm_running, 0);
        EXPECT("compaction: every base from the archive", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled", built0, g_pb_built);
        EXPECT("compaction: every base captured again", descs.count, g_compacted);
        MarkerSerializer *ms = [MarkerSerializer new]; ms->ser = g_ep->ser; g_ep->ser = (id<MTL4PipelineDataSetSerializer>)ms;   // the period being written
        arch_flush_wait();
        EXPECT("compaction: the marker exists while the flush writes", 1, ms->calls == 1 && ms->marked == 1);
        EXPECT("compaction: the flush that returned removed the marker", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(cdir) error:nil];
        EXPECT("compaction: one file left", 1, names.count);
        // a marker left by a compaction that did not finish: the next start on this system build does not compact, one from another build is removed and compaction tried
        EXPECT("compaction: allowed without a marker", 1, arch_compact_allowed());
        [arch_compact_stamp() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: not after one that did not finish on this system build", 0, arch_compact_allowed());
        EXPECT("compaction: that marker stays", 1, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        [[@"os\t1A1\n" stringByAppendingString:@"capture\toriginal-functions-v1\n"] writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: tried again on another system build", 1, arch_compact_allowed());
        EXPECT("compaction: the other build's marker removed", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        [arch_os_build() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil];
        EXPECT("compaction: old archive-import marker permits the new capture method", 1, arch_compact_allowed());
        EXPECT("compaction: old capture marker removed", 0, [NSFileManager.defaultManager fileExistsAtPath:arch_compact_marker()]);
        nvn_hm_clear(&g_pbase, hm_rel); g_compact_enabled = 0; g_compacted = 0;
        g_arch4 = @[[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[@(cdir) stringByAppendingPathComponent:names.firstObject]] error:&err]];
        arch0 = g_pb_archived; built0 = g_pb_built;
        for (MTLRenderPipelineDescriptor *cd in descs) pso_build(cd, &err);
        EXPECT("compaction: the new file holds every base", arch0 + descs.count, g_pb_archived);
        EXPECT("compaction: none compiled from the new file", built0, g_pb_built);
        [NSFileManager.defaultManager removeItemAtPath:@(cdir) error:nil]; g_arch_dir = saved; g_arch4 = @[];
    }
    // a function with no Metal 4 descriptor: the MTLDevice path
    uint64_t legacy = g_pb_legacy;
    id<MTLRenderPipelineState> l = pso_build(desc(MTLPixelFormatRGBA8Unorm, 0, MTLPixelFormatRGBA8Unorm, MTLColorWriteMaskAll, 0), &err);
    EXPECT("unregistered function: a pipeline", 1, l != nil);
    EXPECT("unregistered function: MTLDevice build", legacy + 1, g_pb_legacy);
    depth_only();
    concurrent_bases(0);
    concurrent_bases(1);
    // key normalization
    pso_key k; memset(&k, 0, sizeof k); k.blend_en = 0x5; k.dfmt = MTLPixelFormatDepth32Float; k.sfmt = MTLPixelFormatStencil8;
    for (int i = 0; i < 8; i++) { k.cfmt[i] = MTLPixelFormatRGBA8Unorm; k.blend[i].src_rgb = 5; k.blend[i].dst_rgb = 6; }   // every target present: only the blend rule acts
    pso_key_norm(&k, 0);
    EXPECT("blend of an enabled target kept", 5, k.blend[0].src_rgb);
    EXPECT("blend of an enabled target kept (2)", 6, k.blend[2].dst_rgb);
    EXPECT("blend of a disabled target dropped", 0, k.blend[1].src_rgb);
    EXPECT("blend of a disabled target dropped (7)", 0, k.blend[7].dst_rgb);
    EXPECT("depth format kept (an MTLDevice pipeline names it)", MTLPixelFormatDepth32Float, k.dfmt);
    EXPECT("stencil format kept", MTLPixelFormatStencil8, k.sfmt);
    capture_preserves_foreign_folder();
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_archive: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
