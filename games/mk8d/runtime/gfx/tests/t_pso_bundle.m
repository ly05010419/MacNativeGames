// The pipeline archive made offline and bundled with the app (DM_PSO_BUNDLE, pso_bundle_open / pso_bundle_covers in nvn_mtl.m), made here as build_ipa.sh makes it: tiny shader
// libraries and their reflection, a pair list, then dm/gfx/tools/pso_offline.py (metal-tt) for this Mac's GPU. Checks: the archive for this GPU opens and comes first among the files of
// bases without a vertex layout; with a matching stamp the prebuild builds only the pairs left to the device (a function with constants) and takes nothing from the compiler otherwise,
// and a draw of a covered pair takes its base from the bundle and captures nothing for the device's own archive;
// a stamp of another depth mode or other pairs runs the device's prebuild, which still takes the bundled bases it finds; an archive whose bases the probe does not find is not trusted;
// a folder without this GPU's file and a file that is no archive leave no archive. Without metal-tt (no Metal toolchain) it says so and passes
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

static int sh(NSString *cmd) { int r = system(cmd.UTF8String); return WIFEXITED(r) ? WEXITSTATUS(r) : -1; }
static void put(NSString *path, NSString *text) { [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil]; [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }
static int lib(NSString *dir, NSString *name, NSString *src) {   // <dir>/<name>.metallib from MSL, with the SDK's compiler
    put([NSString stringWithFormat:@"%@/%@.metal", dir, name], src);
    return sh([NSString stringWithFormat:@"cd %@ && xcrun -sdk macosx metal -c %@.metal -o %@.air && xcrun -sdk macosx metallib %@.air -o %@.metallib && rm %@.metal %@.air", dir, name, name, name, name, name, name]);
}
static void reset_counts(void) { g_pre_done = g_pre_built = g_pre_archived = g_pre_skipped = g_pre_failed = 0; }

int main(void) { @autoreleasepool {
    char tmp[] = "/tmp/t_pso_bundle.XXXXXX"; if (!mkdtemp(tmp)) abort();
    NSString *root = @(tmp), *msl = [root stringByAppendingFormat:@"/msl/b-%s", DMSH_CHAIN], *tables = [root stringByAppendingPathComponent:@"tables"];
    NSString *bundle = [root stringByAppendingPathComponent:@"bundle"], *arch = [root stringByAppendingPathComponent:@"archive"];
    // A magic number alone cannot establish an archive: these are the known crash inputs, checked without calling Metal.
    NSString *bad = [root stringByAppendingPathComponent:@"bad.mtl4a"];
    const uint8_t magics[][4] = {{0xcf, 0xfa, 0xed, 0xfe}, {0xcb, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbe}, {0xca, 0xfe, 0xba, 0xbf}};
    for (unsigned i = 0; i < sizeof magics / sizeof magics[0]; i++) {
        [[NSData dataWithBytes:magics[i] length:4] writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare magic refused", 0, arch_file_ok(bad));
        NSMutableData *header = [NSMutableData dataWithLength:32]; memcpy(header.mutableBytes, magics[i], 4); [header writeToFile:bad atomically:YES];
        EXPECT("archive guard: bare header refused", 0, arch_file_ok(bad));
    }
    NSString *v = @"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
        "vertex V main0(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0, 1); return o; }\n"
        "vertex V main0_z(uint id [[vertex_id]]) { V o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * 2 - 1, 0.5, 1); return o; }\n";
    NSString *f = @"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0() { return float4(%d, %d, 0, 1); }\n";
    NSString *fc = @"#include <metal_stdlib>\nusing namespace metal;\nconstant bool k [[function_constant(0)]];\nfragment float4 main0() { return is_function_constant_defined(k) ? float4(1) : float4(0); }\n";
    [NSFileManager.defaultManager createDirectoryAtPath:msl withIntermediateDirectories:YES attributes:nil error:nil];
    if (lib(msl, @"v1_bp", v) || lib(msl, @"f1_b", [NSString stringWithFormat:f, 1, 0]) || lib(msl, @"f2_b", [NSString stringWithFormat:f, 0, 1]) || lib(msl, @"fc_b", fc)) {
        printf("t_pso_bundle: no Metal compiler (xcrun metal), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
    put([msl stringByAppendingPathComponent:@"v1_b.json"], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\", \"entry_z\": \"main0_z\", \"pull\": {}, \"vertex_inputs\": []}", DMSH_CHAIN]);
    for (NSString *k in @[@"f1", @"f2", @"fc"]) put([msl stringByAppendingFormat:@"/%@_b.json", k], [NSString stringWithFormat:@"{\"chain\": \"%s\", \"entry\": \"main0\"}", DMSH_CHAIN]);
    put([tables stringByAppendingPathComponent:@"shader_map.tsv"], @"# empty\n");
    put([tables stringByAppendingPathComponent:@"shader_pairs.tsv"], @"# pairs\nv1\tf1\nv1\tf2\nv1\tfc\n");
    setenv("DM_NVN_SHMAP", [tables stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", [root stringByAppendingPathComponent:@"msl"].UTF8String, 1);
    setenv("DM_PSO_ARCHIVE", arch.UTF8String, 1); setenv("DM_PSO_MANIFEST", "off", 1);
    mtl_init();
    NSString *gpu = g_dev.architecture.name;
    // the archive as build_ipa.sh makes it (the tool's own folder: this file is dm/gfx/tests/t_pso_bundle.m)
    NSString *tool = [[@(__FILE__) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../tools/pso_offline.py"];
    if (sh([NSString stringWithFormat:@"python3 %@ --platform macos --gpu %@ --msl %@ --pairs %@/shader_pairs.tsv --out %@ --work %@/work > %@/pso_offline.log 2>&1",
            tool, gpu, msl, tables, bundle, root, root])) {
        NSString *log = [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"pso_offline.log"] encoding:NSUTF8StringEncoding error:nil];
        if ([log containsString:@"metal-tt is not available"]) { printf("t_pso_bundle: no metal-tt (xcodebuild -downloadComponent MetalToolchain), GPU checks not run; %d file checks failed\n", failed); return failed != 0; }
        fprintf(stderr, "pso_offline.py failed:\n%s\n", log.UTF8String); return 1;
    }
    EXPECT("bundle: this GPU's archive", 1, [NSFileManager.defaultManager fileExistsAtPath:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]]);
    NSData *archiveBytes = [NSData dataWithContentsOfFile:[bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]];
    EXPECT("archive guard: real offline archive accepted", 1, arch_file_ok([bundle stringByAppendingFormat:@"/%@.mtl4a", gpu]));
    for (NSUInteger len = 32; len < archiveBytes.length; len = MIN(len * 2, archiveBytes.length)) {
        [[archiveBytes subdataWithRange:NSMakeRange(0, len)] writeToFile:bad atomically:YES];
        EXPECT("archive guard: truncated offline archive refused", 0, arch_file_ok(bad));
    }
    // Both standard universal wrappers must retain their slice bounds, including 64-bit offsets that cannot fit in the file.
    for (int wide = 0; wide < 2; wide++) {
        NSMutableData *wrapped = [NSMutableData dataWithLength:64];
        struct fat_header *fh = wrapped.mutableBytes; fh->magic = OSSwapHostToBigInt32(wide ? FAT_MAGIC_64 : FAT_MAGIC); fh->nfat_arch = OSSwapHostToBigInt32(1);
        if (wide) {
            struct fat_arch_64 *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt64(64); a->size = OSSwapHostToBigInt64(archiveBytes.length);
        } else {
            struct fat_arch *a = (void *)((uint8_t *)wrapped.mutableBytes + sizeof *fh);
            a->offset = OSSwapHostToBigInt32(64); a->size = OSSwapHostToBigInt32((uint32_t)archiveBytes.length);
        }
        [wrapped appendData:archiveBytes]; [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal GPU image accepted", 1, arch_file_ok(bad));
        if (wide) ((struct fat_arch_64 *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->offset = UINT64_MAX;
        else ((struct fat_arch *)((uint8_t *)wrapped.mutableBytes + sizeof *fh))->size = UINT32_MAX;
        [wrapped writeToFile:bad atomically:YES];
        EXPECT("archive guard: universal slice outside file refused", 0, arch_file_ok(bad));
    }
    NSMutableData *broken = [archiveBytes mutableCopy];
    struct mach_header_64 *mh = broken.mutableBytes;
    EXPECT("archive fixture: one GPU image", MH_MAGIC_64, mh->magic);
    ((struct load_command *)((uint8_t *)broken.mutableBytes + sizeof *mh))->cmdsize = 0;
    [broken writeToFile:bad atomically:YES];
    EXPECT("archive guard: invalid load command refused", 0, arch_file_ok(bad));
    NSString *rest = [NSString stringWithContentsOfFile:[bundle stringByAppendingPathComponent:@"rest_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
    EXPECT("bundle: one pair left to the device", 1, [rest isEqualToString:@"v1\tfc\n"]);
    setenv("DM_PSO_BUNDLE", bundle.UTF8String, 1);
    shmap_load(); pso4_init();
    EXPECT("bundle: opened", 1, g_arch_bundle != nil);
    EXPECT("bundle: first among the files without a vertex layout", 1, g_arch4p.count && g_arch4p[0] == g_arch_bundle);
    // its stamp matches: only the pair with function constants is built here
    reset_counts();
    EXPECT("covered: one pair built on the device", 1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("covered: its base compiled", 1, g_pre_built);
    EXPECT("covered: nothing else", 0, g_pre_archived + g_pre_skipped + g_pre_failed);
    EXPECT("covered: again, nothing to do", -1, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    // a draw of a covered pair (its pulling pipeline, as pso_desc_pull makes it) takes the base from the bundle: nothing compiled, nothing captured for the device's own archive
    @autoreleasepool {
        id<MTLLibrary> vl = lib_from_file([msl stringByAppendingPathComponent:@"v1_bp.metallib"], NULL), fl = lib_from_file([msl stringByAppendingPathComponent:@"f1_b.metallib"], NULL);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = fn4_note([vl newFunctionWithName:@"main0"], vl, @"main0", nil); d.fragmentFunction = fn4_note([fl newFunctionWithName:@"main0"], fl, @"main0", nil);
        d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        uint64_t built0 = g_pb_built, arch0 = g_pb_archived; int new0 = g_arch_new; NSError *err = nil;
        EXPECT("covered draw: a pipeline", 1, pso_build(d, &err) != nil);
        EXPECT("covered draw: its base from the bundle", arch0 + 1, g_pb_archived);
        EXPECT("covered draw: none compiled", built0, g_pb_built);
        EXPECT("covered draw: nothing captured for the device's archive", new0, g_arch_new);
    }
    // the near = 0 depth mode: not what the archive was made for, so the device's prebuild runs, and its main0_z bases are not in the archive
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    reset_counts(); atomic_store(&g_depth_zero, 1);
    EXPECT("other depth mode: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other depth mode: every base compiled", 3, g_pre_built);
    atomic_store(&g_depth_zero, 0);
    // other pairs (a second list): the device's prebuild runs and takes the bundled bases it finds
    [NSFileManager.defaultManager removeItemAtPath:[arch stringByAppendingPathComponent:@"prebuilt.tsv"] error:nil];
    put([tables stringByAppendingPathComponent:@"more.tsv"], @"v1\tf2\nv1\tfc\n"); setenv("DM_PSO_PAIRS", [tables stringByAppendingPathComponent:@"more.tsv"].UTF8String, 1);
    reset_counts();
    EXPECT("other pairs: every pair", 3, pso_prebuild(1, 1, QOS_CLASS_USER_INITIATED));
    EXPECT("other pairs: two bases from the bundle", 2, g_pre_archived);
    EXPECT("other pairs: one compiled", 1, g_pre_built);
    unsetenv("DM_PSO_PAIRS");
    // a matching stamp but an archive without the bases (here: one of the device's own files): the probe finds none, so it is not trusted
    @autoreleasepool {
        NSString *txt = [NSString stringWithContentsOfFile:[tables stringByAppendingPathComponent:@"shader_pairs.tsv"] encoding:NSUTF8StringEncoding error:nil];
        NSString *own = [arch stringByAppendingPathComponent:[[[NSFileManager.defaultManager contentsOfDirectoryAtPath:arch error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'p' AND self ENDSWITH '.mtl4a'"]] firstObject]];
        id<MTL4Archive> real = g_arch_bundle; NSError *e = nil;
        g_arch_bundle = [g_dev newArchiveWithURL:[NSURL fileURLWithPath:own] error:&e];
        EXPECT("archive guard: real serialized archive accepted", 1, arch_file_ok(own));
        EXPECT("probe: another archive opened", 1, g_arch_bundle != nil);
        EXPECT("probe: its bases are not this run's", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        g_arch_bundle = real;
        EXPECT("probe: the bundled archive holds them", 1, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 0), 0, rest));
        EXPECT("probe: not for the other depth mode", 0, pso_bundle_covers(pre_parse(txt), pre_stamp(txt, 1), 1, rest));
        // Raw-pair sampling can hit only ineligible pairs: 0 of 0 lookups must not suppress the device prebuild.
        NSMutableString *sparse = [NSMutableString string];
        for (int i = 0; i < PRE_PROBE; i++) [sparse appendFormat:@"missing%d\tf1\n", i];
        [sparse appendString:@"v1\tf1\n"];
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(sparse, 0));
        EXPECT("probe: no eligible sample cannot establish coverage", 0, pso_bundle_covers(pre_parse(sparse), pre_stamp(sparse, 0), 0, nil));
        put([bundle stringByAppendingPathComponent:@"prebuilt.tsv"], pre_stamp(txt, 0));
    }
    // a folder without this GPU's archive, and a file that is no archive (Metal crashed on one, and on a bare Mach-O header too): no archive, no crash
    @autoreleasepool {
        id<MTL4Archive> real = g_arch_bundle; NSString *other = [root stringByAppendingPathComponent:@"other"];
        put([other stringByAppendingPathComponent:@"applegpu_g99x.mtl4a"], @"x");
        NSMutableArray *list = [NSMutableArray array]; g_arch_bundle = nil;
        setenv("DM_PSO_BUNDLE", other.UTF8String, 1); pso_bundle_open(list);
        EXPECT("no file for this GPU: none opened", 0, list.count + (g_arch_bundle != nil));
        put([other stringByAppendingFormat:@"/%@.mtl4a", gpu], @"not an archive");   // Metal's own loadFromURL crashed on it: never handed to Metal
        pso_bundle_open(list);
        EXPECT("not an archive: none opened", 0, list.count + (g_arch_bundle != nil));
        EXPECT("not an archive: refused before Metal", 0, arch_file_ok([other stringByAppendingFormat:@"/%@.mtl4a", gpu]));
        setenv("DM_PSO_BUNDLE", "off", 1); pso_bundle_open(list);
        EXPECT("off: none opened", 0, list.count);
        g_arch_bundle = real;
    }
    [NSFileManager.defaultManager removeItemAtPath:root error:nil];
    printf("t_pso_bundle: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
