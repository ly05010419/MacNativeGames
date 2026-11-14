// Exercise real manifest loading, shader loading, pipeline builds, cache claims and GPU draws.
// Synthetic shaders need no game files; old rows differ in unused state, while live changes and logical target 7 in a merged pass must remain distinct.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w_ = (want), g_ = (got); checks++; \
    if (w_ != g_) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w_, (unsigned long long)g_); failed++; } } while (0)

static int fixtures(NSString *dir) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * 2 - 1, 0, 1); }\n"
        "fragment float4 fs() { return float4(0.5, 0.25, 1, 0.5); }\n";
    NSString *metal = [dir stringByAppendingPathComponent:@"fixture.metal"];
    NSString *library = [dir stringByAppendingPathComponent:@"warm-vs.metallib"];
    if (![source writeToFile:metal atomically:YES encoding:NSUTF8StringEncoding error:nil]) return 0;
    NSTask *compiler = [NSTask new]; compiler.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
    compiler.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", library];
    NSError *error = nil;
    if (![compiler launchAndReturnError:&error]) { fprintf(stderr, "fixture compiler: %s\n", error.localizedDescription.UTF8String); return 0; }
    [compiler waitUntilExit]; if (compiler.terminationStatus) return 0;
    if (![NSFileManager.defaultManager copyItemAtPath:library toPath:[dir stringByAppendingPathComponent:@"warm-fs.metallib"] error:nil]) return 0;
    for (NSString *stage in @[@"vs", @"fs"]) {
        NSDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"route": @"B", @"entry": stage, @"entry_z": stage, @"bindings": @[], @"vertex_inputs": @[]};
        NSData *json = [NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil];
        if (![json writeToFile:[dir stringByAppendingPathComponent:[NSString stringWithFormat:@"warm-%@.json", stage]] atomically:YES]) return 0;
    }
    g_msl_dir = dir; g_shmap = @{@"warm-vs": @"fixture", @"warm-fs": @"fixture"}.mutableCopy;
    return 1;
}

// Hold the real first library load, not a replacement shader loader. The S and R tasks below
// then reach warm_shader concurrently, while a different shader must still load successfully.
@interface WarmGateDevice : NSObject {
@public id<MTLDevice> device; dispatch_semaphore_t entered, release; _Atomic unsigned library_calls;
}
@end
@implementation WarmGateDevice
- (id)forwardingTargetForSelector:(SEL)selector { return device; }
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    if (!atomic_fetch_add(&library_calls, 1)) {
        dispatch_semaphore_signal(entered);
        dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
    }
    return [device newLibraryWithData:data error:error];
}
@end

// Observe the second caller's lookup under the production dictionary lock. Even if the test
// releases the device gate immediately, the owner cannot publish until this caller unlocks.
@interface WarmLookupDictionary : NSObject {
@public NSMutableDictionary *dictionary; dispatch_semaphore_t second_lookup; unsigned lookups;
}
@end
@implementation WarmLookupDictionary
- (id)forwardingTargetForSelector:(SEL)selector { return dictionary; }
- (id)objectForKeyedSubscript:(id)key {
    if ([key isEqual:@"warm-vs"] && ++lookups == 2) dispatch_semaphore_signal(second_lookup);
    return dictionary[key];
}
@end

static NSString *row(const pso_key *key, int merged) {
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    strcpy(vs->key, "warm-vs"); strcpy(fs->key, "warm-fs"); fs->stage = 1;
    pso_skey sk; char hex[sizeof sk * 2 + 1];
    pso_skey_from(key, vs, fs, merged, NULL, &sk); pso_skey_hex(&sk, hex);
    return [NSString stringWithFormat:@"R\t%s\n", hex];
}

static void pixels(id<MTLRenderPipelineState> pipeline, uint8_t out[4]) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new]; pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear; pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandQueue> queue = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:pass];
    [enc setRenderPipelineState:pipeline]; [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [enc endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    EXPECT("GPU draw completed", MTLCommandBufferStatusCompleted, cb.status);
    [target getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
}

static void check_draw(const pso_key *raw, int merged, const uint8_t expected[4]) {
    NvShader *vs = g_shcache[@"warm-vs"], *fs = g_shcache[@"warm-fs"];
    if (!vs || !fs) { EXPECT("warmed shaders claimed", 1, 0); return; }
    state_reset(); S.color.blend_en = raw->blend_en; S.chmask.mask = raw->chmask; S.ms.a2c = raw->a2c;
    memcpy(S.blend, raw->blend, sizeof S.blend);
    memcpy(g_pass.cfmt, raw->cfmt, sizeof g_pass.cfmt); g_pass.dfmt = raw->dfmt; g_pass.sfmt = raw->sfmt; g_pass.samples = raw->samples;
    g_pso_lmap = merged ? raw->lmap : NULL;
    pso_key live; pso_key_fill(&live, vs, fs, raw->cfmt, raw->dfmt, raw->sfmt, raw->samples, merged);
    if (merged) memcpy(live.lmap, raw->lmap, sizeof live.lmap);
    EXPECT("live key finds warmed pipeline", 1, hm_get_id(&g_pso, &live) != nil);
    uint64_t before = g_pb_spec + g_pb_legacy;
    NvProg *program = [NvProg new]; program->sh[0] = vs; program->sh[1] = fs;
    id<MTLRenderPipelineState> warmed = pso_get_(program, nil);
    EXPECT("draw lookup adds no pipeline build", before, g_pb_spec + g_pb_legacy);
    EXPECT("draw lookup returns a pipeline", 1, warmed != nil);
    if (!warmed) { g_pso_lmap = NULL; return; }
    // Build the descriptor from the unnormalized historical state as a rendering oracle.
    pso_key original = *raw; original.vs = (__bridge void *)vs; original.fs = (__bridge void *)fs;
    NSError *error = nil; id<MTLRenderPipelineState> reference = pso_build(pso_desc(&original, vs, fs, nil, merged), &error);
    EXPECT("original descriptor builds", 1, reference != nil);
    if (reference) {
        uint8_t a[4], b[4]; pixels(warmed, a); pixels(reference, b);
        EXPECT("normalization preserves rendered pixels", 1, memcmp(a, b, sizeof a) == 0);
        int correct = 1; for (int i = 0; i < 4; i++) correct &= abs((int)a[i] - expected[i]) <= 1;
        EXPECT("pixels have the intended blend and channel mask", 1, correct);
    }
    g_pso_lmap = NULL;
}

static void singleflight(NSString *dir) {
    warm_load_begin(); g_warm_items = [NSMutableArray array]; atomic_store(&g_warm_ready, 0);
    WarmLookupDictionary *lookup = [WarmLookupDictionary new]; lookup->dictionary = g_warm_dict;
    lookup->second_lookup = dispatch_semaphore_create(0); g_warm_dict = (NSMutableDictionary *)lookup;
    for (NSString *extension in @[@"json", @"metallib"]) {
        NSString *source = [dir stringByAppendingPathComponent:[@"warm-fs." stringByAppendingString:extension]];
        NSString *target = [dir stringByAppendingPathComponent:[@"warm-other." stringByAppendingString:extension]];
        if (![NSFileManager.defaultManager fileExistsAtPath:target])
            EXPECT("unrelated fixture copied", 1, [NSFileManager.defaultManager copyItemAtPath:source toPath:target error:nil]);
    }
    pso_key key = {0}; key.cfmt[0] = MTLPixelFormatRGBA8Unorm; key.samples = 1; key.chmask = 15;
    NSString *manifest = [dir stringByAppendingPathComponent:@"concurrent.tsv"];
    NSString *text = [@"S\twarm-vs\t0\n" stringByAppendingString:row(&key, 0)];
    EXPECT("concurrent manifest written", 1, [text writeToFile:manifest atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(@[manifest], &lines);
    EXPECT("S and R are distinct real warm tasks", 2, tasks.count);
    if (tasks.count != 2) return;
    pso4_init();   // Match warm_start: initialize the real compiler before substituting the test device.
    WarmGateDevice *gate = [WarmGateDevice new]; gate->device = g_dev;
    gate->entered = dispatch_semaphore_create(0); gate->release = dispatch_semaphore_create(0);
    g_dev = (id<MTLDevice>)gate;
    dispatch_group_t callers = dispatch_group_create();
    dispatch_group_async(callers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ warm_task(tasks[0]); });
    EXPECT("first real shader load reaches device gate", 0, dispatch_semaphore_wait(gate->entered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_group_async(callers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ warm_task(tasks[1]); });
    EXPECT("R task reaches S task's key while its load is held", 0, dispatch_semaphore_wait(lookup->second_lookup, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    // A dedicated worker avoids a failed assertion hanging the test behind the very global load lock it is meant to catch.
    dispatch_group_t unrelated = dispatch_group_create();
    __block NvShader *other = nil;
    dispatch_group_async(unrelated, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { other = warm_shader(@"warm-other", 1); } });
    EXPECT("unrelated shader finishes before owner is released", 0, dispatch_group_wait(unrelated, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)));
    dispatch_semaphore_signal(gate->release);
    dispatch_group_wait(callers, DISPATCH_TIME_FOREVER); dispatch_group_wait(unrelated, DISPATCH_TIME_FOREVER);
    EXPECT("unrelated shader loaded", 1, other != nil);
    EXPECT("actual library loads", 3, atomic_load(&gate->library_calls));
    NvShader *published = warm_shader(@"warm-vs", 0);
    unsigned references = 0;
    for (WarmItem *item in g_warm_items) {
        if (item->kind == 0) { EXPECT("S task retains the published shader", 1, item->shader == published); references++; }
        if (item->kind == 1) { EXPECT("R task retains the published shader", 1, item->vs == published); references++; }
    }
    EXPECT("both real callers returned the same shader", 2, references);
    EXPECT("cached lookup returns the published shader", 1, warm_shader(@"warm-vs", 0) == published);
    EXPECT("cached lookups perform no library loads", 3, atomic_load(&gate->library_calls));
    g_dev = gate->device;
    warm_claim();
    NvShader *vs = g_shcache[@"warm-vs"];
    EXPECT("S and R publish one shader identity", 1, vs == published);
    // Missing geometry reflection returns nil quietly. Warm callers share its negative result;
    // a later demand caller still retries, and succeeds once real reflection appears.
    EXPECT("failed warm load", 1, warm_shader(@"retry-gs", 2) == nil);
    NSDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"gs_slice": @{@"loc": @0, @"comp": @"x"}};
    NSString *path = [dir stringByAppendingPathComponent:@"retry-gs.json"];
    EXPECT("retry reflection written", 1, [[NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil] writeToFile:path atomically:YES]);
    EXPECT("failure stays cached for this warm phase", 1, warm_shader(@"retry-gs", 2) == nil);
    EXPECT("demand loading still retries failed warm key", 1, shader_get(@"retry-gs", 2) != nil);
    EXPECT("retry fixture removed", 1, [NSFileManager.defaultManager removeItemAtPath:path error:nil]);
    g_warm_t0 = bp_now(); warm_finish();
    g_shcache = nil; nvn_hm_clear(&g_pso, hm_rel);
}

static void scenario(NSString *dir, int merged) {
    nvn_hm_clear(&g_pso, hm_rel); g_shcache = nil; g_warm_items = [NSMutableArray array];
    warm_load_begin();
    g_warm_rps = g_warm_claim_rps = 0; atomic_store(&g_warm_ready, 0);
    pso_key keys[4] = {0}; keys[0].cfmt[0] = MTLPixelFormatRGBA8Unorm; keys[0].samples = 1;
    int logical = merged ? 7 : 0;
    keys[0].chmask = 0xfu << (4 * logical);
    blend_st enabled = {.src_rgb = NVN_BLEND_FUNC_SRC_ALPHA, .dst_rgb = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
        .src_a = NVN_BLEND_FUNC_SRC_ALPHA, .dst_a = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, .eq_rgb = NVN_BLEND_EQUATION_ADD, .eq_a = NVN_BLEND_EQUATION_ADD};
    if (merged) { memset(keys[0].lmap, 0xff, sizeof keys[0].lmap); keys[0].lmap[0] = 7; keys[0].blend_en = 1u << 7; keys[0].blend[7] = enabled; }
    keys[1] = keys[0];
    // Neither row matches byte-for-byte, but both describe the same pipeline.
    if (merged) keys[1].blend[1].src_rgb = NVN_BLEND_FUNC_DST_ALPHA;
    else { keys[1].blend_en = 1u << 7; keys[1].chmask |= 0xfu << 28; }   // old pso_key_fill copied no blend factors for absent targets
    keys[2] = keys[0]; keys[2].chmask = 2u << (4 * logical);   // a real write-mask difference
    keys[3] = keys[0]; keys[3].blend_en ^= 1u << logical;   // a real blend-enable difference
    if (!merged) keys[3].blend[0] = enabled;
    NSString *a = [dir stringByAppendingPathComponent:@"seed.tsv"], *b = [dir stringByAppendingPathComponent:@"user.tsv"];
    NSString *r0 = row(&keys[0], merged), *r1 = row(&keys[1], merged);
    EXPECT("historical rows differ before loading", 0, [r0 isEqual:r1]);
    NSString *seed = [r0 stringByAppendingString:r1];
    // The second list repeats both old rows, then changes two pieces of effective state.
    NSString *user = [NSString stringWithFormat:@"%@%@%@%@", r1, r0, row(&keys[2], merged), row(&keys[3], merged)];
    EXPECT("seed manifest written", 1, [seed writeToFile:a atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    EXPECT("user manifest written", 1, [user writeToFile:b atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    uint64_t before = g_pb_spec + g_pb_legacy; g_warm_t0 = bp_now(); atomic_store(&g_warm_running, 1);
    long lines = 0; for (WarmTask *t in warm_parse(@[a, b], &lines)) warm_task(t);
    warm_finish();
    EXPECT("only three effective pipelines compiled across both manifests", 3, g_pb_spec + g_pb_legacy - before);
    EXPECT("only three render pipelines enqueued", 3, g_warm_rps);
    warm_claim();
    EXPECT("three warmed pipelines claimed into cache", 3, g_warm_claim_rps);
    EXPECT("cache has exactly three effective keys", 3, g_pso.n);
    const uint8_t plain[4] = {128, 64, 255, 128}, green[4] = {255, 64, 0, 255};
    const uint8_t blended[4] = {191, 32, 128, 191}, blended_green[4] = {255, 32, 0, 255};
    check_draw(&keys[0], merged, merged ? blended : plain);
    check_draw(&keys[1], merged, merged ? blended : plain);
    check_draw(&keys[2], merged, merged ? blended_green : green);
    check_draw(&keys[3], merged, merged ? plain : blended);
    printf("%s manifests: six rows -> %llu builds, %u cache keys\n", merged ? "merged" : "normal", (unsigned long long)g_warm_rps, g_pso.n);
}

int main(void) { @autoreleasepool {
    alarm(60);   // A regression in owner/waiter progress must fail rather than hang the suite.
    setenv("DM_PSO_ARCHIVE", "off", 1); setenv("DM_NVN_PREDICT", "0", 1);
    unsetenv("DM_NVN_HILITE"); unsetenv("DM_NVN_MAGSCAN"); unsetenv("DM_PSO_RECORD"); unsetenv("DM_PSO_USER"); unsetenv("DM_SAVE");
    char dir[] = "/tmp/t_pso_warm.XXXXXX"; if (!mkdtemp(dir)) return 1;
    mtl_init();
    if (!fixtures(@(dir))) { fprintf(stderr, "failed to compile synthetic shader fixtures\n"); [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil]; return 1; }
    singleflight(@(dir));
    scenario(@(dir), 0); scenario(@(dir), 1);
    const char *mode = getenv("DM_PSO_MTL4");
    if (mode) EXPECT("requested compiler path selected", strcmp(mode, "0") != 0, g_comp4 != nil);
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    printf("t_pso_warm: %d checks, %d failed; %s\n", checks, failed, g_comp4 ? "Metal 4" : "MTLDevice");
    return failed != 0;
} }
