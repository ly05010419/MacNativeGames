// Reading the shader map is one thing and starting the prewarm another. The map is read once from any thread (shmap_read_once: the prefetch at nvnProgramSetShaders, the submit thread), inside a
// dispatch_once; the prewarm and its prepare step run on the submit path, at the first submission, and reach shmap_load again (pso_prebuild). With the prewarm inside the once, that second call on
// the same thread trapped in _dispatch_once_wait: every game with a window and a prebuild, at its first program.
#include "../nvn/nvn_mtl.m"
#include <stdlib.h>
#include <unistd.h>

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int g_checks, g_failed;
#define EXPECT_EQ(name, want, got) do { \
    long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%s expected %lld -> got %lld %s\n", name, w_, g_, w_ == g_ ? "OK" : "FAIL"); \
    g_failed += w_ != g_; \
} while (0)

static void put(NSString *dir, NSString *name, NSString *text) {
    if (![text writeToFile:[dir stringByAppendingPathComponent:name] atomically:YES encoding:NSUTF8StringEncoding error:nil]) { printf("cannot write %s\n", name.UTF8String); exit(2); }
}

int main(void) { @autoreleasepool {
    char tmpl[] = "/tmp/t_shmap_warm.XXXXXX";
    if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 2; }
    NSString *dir = @(tmpl);
    put(dir, @"shader_map.tsv", @"# no shaders\n");
    put(dir, @"pso_manifest.tsv", @"# an empty seed list: the prepare step goes on to the prebuild\n");
    setenv("DM_NVN_SHMAP", [dir stringByAppendingPathComponent:@"shader_map.tsv"].UTF8String, 1);
    setenv("DM_MSL_DIR", tmpl, 1);
    setenv("DM_PSO_PREPARE", "1", 1);   // as with a window (front_metal_layer here has none)
    unsetenv("DM_PSO_MANIFEST"); unsetenv("DM_SAVE"); unsetenv("DM_PSO_PREBUILD"); unsetenv("DM_PSO_HYBRID");
    mtl_init();

    shmap_read_once();   // what the prefetch does at the game's first program
    EXPECT_EQ("the map is read without starting the prewarm", 0, g_warm_started);

    warm_enqueue([WarmItem new]);   // a library the prefetch finished before the first submission
    EXPECT_EQ("an item finished before the prewarm starts is kept", 1, g_warm_items.count);

    mtl_prepare();   // the first submission: the prewarm, then the prebuild, which calls shmap_load again on this thread
    EXPECT_EQ("the first submission starts the prewarm", 1, g_warm_started);
    EXPECT_EQ("the prebuild path was reached: the compiler it needs exists", 1, hybrid_on());
    EXPECT_EQ("the item is still there", 1, g_warm_items.count);

    mtl_prepare(); shmap_load();   // later calls: nothing starts twice
    EXPECT_EQ("the prewarm starts once", 1, g_warm_started);

    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
} }
