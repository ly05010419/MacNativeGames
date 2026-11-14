// In-run double-render regression: goes directly through production fz_end / fz_check_run, with real 16×16 Metal attachments.
// The later pass's logical c0 is at physical c3 of the union; the expected red and blue values are defined by constants, not computed via the implementation under test.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int fail, count;
#define EXPECT(name, want, got) do { uint64_t w = (want), v = (got); count++; \
    printf("%s expected %llu → got %llu %s\n", name, (unsigned long long)w, \
           (unsigned long long)v, w == v ? "✓" : "✗"); fail += w != v; } while (0)

static void check_color(int blue, const prog_t *pg, tex_t t[4]) {
    uint64_t same = g_fzc_same, diff = g_fzc_diff;
    state_reset(); S.prog = pg;
    memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 4;
    for (int i = 0; i < 4; i++) R.rt.color[i] = &t[i];
    R.clear_c[3] = 1; R.cc[3] = MTLClearColorMake(!blue, 0, blue, 1);
    memset(&g_fz, 0, sizeof g_fz);
    g_fz.active = 1; g_fz.phase = 2; g_fz.n1 = 3;
    g_fz.rt2.n = 1; g_fz.rt2.color[0] = &t[3];
    memset(g_fz.p2map, -1, sizeof g_fz.p2map); g_fz.p2map[0] = 3;
    g_fz.wrote[3] = 1;
    p_draw dw = {NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3};
    fz_snap_take(&dw, NULL);
    mtl_sub_begin(); rp_begin_ex(1); rp_end();
    uint64_t f = mtl_sub_commit();
    EXPECT("GPU done", 1, mtl_wait(f, 5000000000ull));
    EXPECT(blue ? "negative control: blue merged result vs red plain result, really compared as different attachments" : "physical c3 → logical c0, really compared as the same red attachment",
           1, blue ? g_fzc_diff - diff : g_fzc_same - same);
    EXPECT("exactly one comparison recorded for this case, none missed or duplicated", 1, g_fzc_same - same + g_fzc_diff - diff);
    EXPECT("the later pass's logical target is kept afterwards", 1, R.rt.n == 1 && R.rt.color[0] == &t[3]);
    EXPECT("snapshot released", 0, g_nfzs);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_FUSE_CHECK", "1", 1); mtl_init();
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n [[vertex_id]]) { float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[n],0,1); }\n"
        "fragment float4 f() { return float4(1,0,0,1); }\n" options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    NvProg *pr = [NvProg new];
    pr->sh[0] = [NvShader new]; pr->sh[0]->fn = [lib newFunctionWithName:@"v"];
    pr->sh[1] = [NvShader new]; pr->sh[1]->fn = [lib newFunctionWithName:@"f"]; pr->sh[1]->stage = 1;
    prog_t pg = {.serial = 100}; serial_put(pg.serial, pr);
    tex_t t[4] = {0};
    for (int i = 0; i < 4; i++) { t[i].magic = MAGIC('T', 'E', 'X', 0); t[i].serial = 101 + i; t[i].d.width = t[i].d.height = 16;
        t[i].d.depth = t[i].d.levels = t[i].d.samples = 1; t[i].d.target = NVN_TEXTURE_TARGET_2D; t[i].d.format = NVN_FORMAT_RGBA8; }
    check_color(0, &pg, t); check_color(1, &pg, t);
    printf("== %d checks, %d failed ==\n", count, fail); return fail != 0;
} }
