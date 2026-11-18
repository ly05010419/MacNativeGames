// Packaging's real manifest compiler, tested in fresh processes without game data.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static void put(NSString *path, NSString *text) {
    if (![text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]) exit(2);
}
int main(int argc, char **argv) { @autoreleasepool {
    if (argc == 3 && !strcmp(argv[1], "--fixture")) {
        NSString *root = @(argv[2]);
        NSString *lib = [root stringByAppendingPathComponent:@"cache/b-" DMSH_CHAIN];
        [NSFileManager.defaultManager createDirectoryAtPath:lib withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *metal = [root stringByAppendingPathComponent:@"fixture.metal"];
        NSString *binary = [lib stringByAppendingPathComponent:@"0000000000000001_b.metallib"];
        put(metal, @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 vs(uint id [[vertex_id]]) { return float4(float(id),0,0,1); }\n"
            "fragment float4 fs() { return float4(1,0,0,1); }\n");
        NSTask *compiler = [NSTask new]; compiler.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"];
        compiler.arguments = @[@"-sdk", @"macosx", @"metal", metal, @"-o", binary];
        if (![compiler launchAndReturnError:nil]) return 2;
        [compiler waitUntilExit]; if (compiler.terminationStatus) return 2;
        [NSFileManager.defaultManager copyItemAtPath:binary toPath:[lib stringByAppendingPathComponent:@"0000000000000002_b.metallib"] error:nil];
        for (int i = 1; i <= 2; i++) {
            NSDictionary *reflection = @{@"chain": @DMSH_CHAIN, @"route": @"B", @"entry": i == 1 ? @"vs" : @"fs",
                @"entry_z": i == 1 ? @"vs" : @"fs", @"bindings": @[], @"vertex_inputs": @[]};
            NSData *data = [NSJSONSerialization dataWithJSONObject:reflection options:0 error:nil];
            [data writeToFile:[lib stringByAppendingPathComponent:[NSString stringWithFormat:@"%016d_b.json", i]] atomically:YES];
        }
        put([root stringByAppendingPathComponent:@"shader_map.tsv"],
            @"fixture\t0\tvertex\t0000000000000001\nfixture\t1\tpixel\t0000000000000002\n");
        NvShader *vs = [NvShader new], *fs = [NvShader new];
        strcpy(vs->key, "0000000000000001_b"); strcpy(fs->key, "0000000000000002_b"); fs->stage = 1;
        pso_key k = {0}; k.samples = 1; k.cfmt[0] = MTLPixelFormatRGBA8Unorm; k.chmask = 15;
        pso_skey sk; char hex[sizeof sk * 2 + 1];
        pso_skey_from(&k, vs, fs, 0, NULL, &sk); pso_skey_hex(&sk, hex);
        put([root stringByAppendingPathComponent:@"pso_manifest.tsv"], [NSString stringWithFormat:@"R\t%s\n", hex]);
        return 0;
    }
    if (!mtl_pso_prebuild_tool()) return 3;
    if (getenv("DM_TEST_SEED") && (g_pb_built || !g_pb_archived)) {
        fprintf(stderr, "seed reuse failed: %llu compiled, %llu archived\n", (unsigned long long)g_pb_built, (unsigned long long)g_pb_archived);
        return 4;
    }
    fprintf(stdout, "manifest probe passed: %llu compiled, %llu archived\n", (unsigned long long)g_pb_built, (unsigned long long)g_pb_archived);
    return 0;
} }
