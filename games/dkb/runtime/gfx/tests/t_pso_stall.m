// The prebuild with a base Apple's compiler cannot build (GitHub issue #294), stood in for by a compiler that holds one pixel shader's compile, and one that fails another's.
// A compile that does not return holds back only the file of its own flush period (ArchEpoch in nvn_mtl.m). Checks, while it is held: the prebuild hands out every pair (where the
// prepare step stops waiting) and builds the others; a draw's compile goes through; a flush of a later period writes its file. Once released: its period is written, the prebuild
// finishes and stamps, nothing is left to write. A base that fails (pre_fail_load): the stamp is written and prebuild_failed.tsv names it; a later start of this system build
// with that stamp does nothing, and without the stamp it leaves the pair to the draws without compiling it; a list written on another system build makes it try the pair again
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, ...) do { uint64_t w = (want), g = (__VA_ARGS__); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

// Holds the compile whose pixel function is named "hold" until released, and fails the one named "bad" (bad: how many times it was asked)
@interface HoldCompiler : NSObject { @public id<MTL4Compiler> compiler; dispatch_semaphore_t entered, release; _Atomic int bad; } @end
@implementation HoldCompiler
- (id)forwardingTargetForSelector:(SEL)selector { return compiler; }
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTL4RenderPipelineDescriptor *)d compilerTaskOptions:(MTL4CompilerTaskOptions *)opts error:(NSError **)error {
    MTL4FunctionDescriptor *f = d.fragmentFunctionDescriptor;
    if ([f isKindOfClass:[MTL4LibraryFunctionDescriptor class]] && [((MTL4LibraryFunctionDescriptor *)f).name isEqualToString:@"hold"]) {
        dispatch_semaphore_signal(entered); dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
    }
    if ([f isKindOfClass:[MTL4LibraryFunctionDescriptor class]] && [((MTL4LibraryFunctionDescriptor *)f).name isEqualToString:@"bad"]) {
        bad++; if (error) *error = [NSError errorWithDomain:@"test" code:1 userInfo:@{NSLocalizedDescriptionKey: @"Compilation failed\tafter\nmultiple retries"}]; return nil;
    }
    return [compiler newRenderPipelineStateWithDescriptor:d compilerTaskOptions:opts error:error];
}
@end
static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL, with the SDK's compiler
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd %@ && xcrun -sdk macosx metal -c %@.metal -o %@.air && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
static NSUInteger files(NSString *dir) { return [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]].count; }
static void reset_counts(void) { g_pre_done = g_pre_built = g_pre_archived = g_pre_skipped = g_pre_failed = 0; }
static NSString *slurp(NSString *path) { return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil]; }
static void arm(HoldCompiler *c) { if (![(id)g_ep->comp isKindOfClass:[HoldCompiler class]]) { c->compiler = g_ep->comp; g_ep->comp = (id<MTL4Compiler>)c; } }   // the current period's compiler
static void reopen(NSString *dir) {   // the folder's files, as a new start opens them
    NSMutableArray<id<MTL4Archive>> *a = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil]) if ([n hasSuffix:@".mtl4a"]) [a addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[dir stringByAppendingPathComponent:n]] error:nil]];
    g_arch4p = a;
}
static int until(int (^cond)(void), double seconds) { uint64_t t0 = bp_now(); while (!cond()) { if (bp_now() - t0 > seconds * 1e9) return 0; usleep(10000); } return 1; }

int main(void) { @autoreleasepool {
    char tmp[] = "/tmp/t_pso_stall.XXXXXX"; if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *msl = [root stringByAppendingFormat:@"/msl/b-%s", DMSH_CHAIN], *tables = [root stringByAppendingPathComponent:@"tables"];
    NSString *arch = [root stringByAppendingPathComponent:@"archive"];
    NSString *v = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n";
    NSString *f = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 %@() { return float4(%d, %d, %d.0 / 65536.0, 1); }\n";
    [NSFileManager.defaultManager createDirectoryAtPath:msl withIntermediateDirectories:YES attributes:nil error:nil];
    int pid = getpid() & 0xffff;
    if (lib(msl, @"v1_bp", v) || lib(msl, @"f1_b", [NSString stringWithFormat:f, @"hold", 1, 0, pid]) || lib(msl, @"f2_b", [NSString stringWithFormat:f, @"main0", 0, 1, pid]) ||
        lib(msl, @"f3_b", [NSString stringWithFormat:f, @"main0", 1, 1, pid]) || lib(msl, @"f4_b", [NSString stringWithFormat:f, @"main0", 0, 0, pid]) ||
        lib(msl, @"f5_b", [NSString stringWithFormat:f, @"bad", 2, 0, pid]) || lib(msl, @"f6_b", [NSString stringWithFormat:f, @"main0", 0, 2, pid])) {
        printf("t_pso_stall: no Metal compiler (xcrun metal), not run\n"); return 0; }
    put([msl stringByAppendingPathComponent:@"v1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"pull\": {}, \"vertex_inputs\": []}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"f1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"hold\"}", DMSH_CHAIN]);
    put([msl stringByAppendingPathComponent:@"f5_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"bad\"}", DMSH_CHAIN]);
    for (NSString *k in @[@"f2", @"f3", @"f4", @"f6"]) put([msl stringByAppendingFormat:@"/%@_b.json", k], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\"}", DMSH_CHAIN]);
    put([tables stringByAppendingPathComponent:@"shader_map.tsv"], @"# empty\n");
    put([tables stringByAppendingPathComponent:@"shader_pairs.tsv"], @"# pairs\nv1\tf1\nv1\tf2\nv1\tf3\n");
    setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1);
    setenv("DM_PSO_ARCHIVE", arch.UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init(); pso4_init();
    if (!g_comp4 || !g_arch_cap) { printf("t_pso_stall: no Metal 4 compiler, not run\n"); return 0; }

    ArchEpoch *ep1 = g_ep; HoldCompiler *hold = [HoldCompiler new]; hold->compiler = ep1->comp;
    hold->entered = dispatch_semaphore_create(0); hold->release = dispatch_semaphore_create(0); ep1->comp = (id<MTL4Compiler>)hold;
    EXPECT("three pairs", 3, pso_prebuild(2, 0, QOS_CLASS_USER_INITIATED));
    EXPECT("held: the compile of f1 entered", 0, dispatch_semaphore_wait(hold->entered, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)));
    EXPECT("held: every pair handed out (the prepare step stops waiting)", 1, until(^{ return (int)(atomic_load(&g_pre_next) >= 3); }, 10));
    EXPECT("held: the other pairs built", 1, until(^{ return (int)(g_pre_done == 2); }, 10));
    EXPECT("held: the prebuild not done", 1, g_pre_running);

    // a flush hands the held period over and returns; a draw's compile in the next period goes through, and that period's flush writes its file
    arch_flush();
    EXPECT("held: the flush began a new period", 1, g_ep != ep1);
    MTL4RenderPipelineDescriptor *m = pre_desc(@"v1", @"f4", 0);
    EXPECT("held: a descriptor for the draw", 1, m != nil);
    dispatch_group_t draw = dispatch_group_create(); __block id drawn = nil;
    dispatch_group_async(draw, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool { NSError *e = nil; drawn = arch_capture(m, &e, 0); } });
    EXPECT("held: a draw's compile returns", 0, dispatch_group_wait(draw, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)));
    EXPECT("held: a draw's compile built", 1, drawn != nil);
    arch_flush();
    EXPECT("held: the later period's file written", 1, until(^{ return (int)(files(arch) == 1); }, 10));

    // released: the held period is written, the prebuild finishes and stamps
    dispatch_semaphore_signal(hold->release);
    EXPECT("released: the prebuild finished", 1, until(^{ return (int)!g_pre_running; }, 30));
    EXPECT("released: every pair built", 3, g_pre_built);
    EXPECT("released: none failed", 0, g_pre_failed);
    EXPECT("released: the held period's file written", 2, files(arch));
    EXPECT("released: stamped", 1, [NSFileManager.defaultManager fileExistsAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"]]);
    EXPECT("released: nothing left to write", 0, g_arch_new);
    ep1->comp = hold->compiler;

    // every base is in the files
    NSMutableArray<id<MTL4Archive>> *open = [NSMutableArray array];
    for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:arch error:nil]) if ([n hasSuffix:@".mtl4a"])
        [open addObject:[g_dev newArchiveWithURL:[NSURL fileURLWithPath:[arch stringByAppendingPathComponent:n]] error:nil]];
    int found = 0;
    for (NSString *k in @[@"f1", @"f2", @"f3", @"f4"]) @autoreleasepool {
        MTL4RenderPipelineDescriptor *d = pre_desc(@"v1", k, 0);
        for (id<MTL4Archive> a in open) if ([a newRenderPipelineStateWithDescriptor:d error:nil]) { found++; break; }
    }
    EXPECT("every base in the files", 4, found);
    // a base whose period is written and gone still specializes (the draws keep bases for the whole run)
    MTL4RenderPipelineDescriptor *s = (MTL4RenderPipelineDescriptor *)[drawn newRenderPipelineDescriptorForSpecialization];
    s.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; s.colorAttachments[0].writeMask = MTLColorWriteMaskAll; s.colorAttachments[0].blendingState = MTL4BlendStateDisabled;
    NSError *se = nil;
    EXPECT("a base of a written period specializes", 1, [g_comp4 newRenderPipelineStateBySpecializationWithDescriptor:s pipeline:drawn error:&se] != nil);

    // a base that fails: into a fresh archive folder, with new pairs (one fails, one builds)
    NSString *arch2 = [root stringByAppendingPathComponent:@"archive2"], *stamp = [arch2 stringByAppendingPathComponent:@"prebuilt.tsv"], *failp = [arch2 stringByAppendingPathComponent:@"prebuild_failed.tsv"];
    [NSFileManager.defaultManager createDirectoryAtPath:arch2 withIntermediateDirectories:YES attributes:nil error:nil];
    g_arch_dir = arch2; g_arch4 = @[]; g_arch4p = @[];
    put([tables stringByAppendingPathComponent:@"shader_pairs.tsv"], @"# pairs\nv1\tf5\nv1\tf6\n");
    HoldCompiler *failc = [HoldCompiler new];   // armed in the current period before each run (one worker: both compiles in it; the run's last flush begins another)
    arm(failc); reset_counts();
    EXPECT("failing: two pairs", 2, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("failing: asked once", 1, failc->bad);
    EXPECT("failing: one failed", 1, g_pre_failed);
    EXPECT("failing: one built", 1, g_pre_built);
    EXPECT("failing: stamped all the same", 1, slurp(stamp) != nil);
    NSString *list = slurp(failp);
    EXPECT("failing: the failure list names the pair", 1, [list containsString:@"\nfail\tv1\tf5\tCompilation failed after multiple retries\n"]);
    EXPECT("failing: for this system build", 1, [list containsString:arch_os_build()]);
    // a later start of this system build with that stamp: nothing to do
    reset_counts();
    EXPECT("again: nothing to do", (uint64_t)-1, (uint64_t)(int64_t)pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    // without the stamp: the failed pair is left to the draws, not compiled again
    unlink(stamp.fileSystemRepresentation); reopen(arch2); arm(failc); reset_counts();
    EXPECT("unstamped: two pairs", 2, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("unstamped: not asked again", 1, failc->bad);
    EXPECT("unstamped: left to the draws", 1, g_pre_skipped);
    EXPECT("unstamped: as one that failed before", 1, g_pre_known_n);
    EXPECT("unstamped: the other from the archive", 1, g_pre_archived);
    EXPECT("unstamped: none failed this time", 0, g_pre_failed);
    EXPECT("unstamped: the list keeps it", 1, [slurp(failp) containsString:@"fail\tv1\tf5\t"]);
    // a list written on another system build: tried again
    put(failp, [slurp(failp) stringByReplacingOccurrencesOfString:arch_os_build() withString:@"os\t1A1\n"]); reopen(arch2); arm(failc); reset_counts();
    EXPECT("other build: two pairs", 2, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other build: asked again", 2, failc->bad);
    EXPECT("other build: failed again", 1, g_pre_failed);
    EXPECT("other build: the other from the archive", 1, g_pre_archived);
    EXPECT("other build: the list is for this build now", 1, [slurp(failp) containsString:arch_os_build()]);
    // it builds after a system update (the real compiler, the list from another build): the list goes
    if ((id)g_ep->comp == failc) g_ep->comp = failc->compiler;
    put(failp, [slurp(failp) stringByReplacingOccurrencesOfString:arch_os_build() withString:@"os\t1A1\n"]); reopen(arch2); reset_counts();
    EXPECT("fixed: two pairs", 2, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("fixed: built", 1, g_pre_built);
    EXPECT("fixed: no failure list", 0, [NSFileManager.defaultManager fileExistsAtPath:failp]);
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_stall: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
