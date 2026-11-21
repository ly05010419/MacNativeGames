// Registered texture views use only the mip levels present in the texture; exercise the production Metal path.
// Texture fields Metal refuses (tex_desc): levels past what the size holds, layer counts of 0 or past 2048, sample counts the GPU lacks; copies at a missing level are skipped.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void view_case(int tid, unsigned level, unsigned count, unsigned expected, int layers) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=97000+(unsigned)tid};
    t.d = (nvn_texdesc){.target=layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D,
                       .format=NVN_FORMAT_RGBA8, .width=64, .height=64, .depth=layers, .levels=7, .samples=1};
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:tex_desc(&t, 0)];
    CHECK(base && base.mipmapLevelCount == 7, "base has seven levels");
    serial_put(t.serial, base);
    view_t v = {.set_levels=1, .base_level=level, .nlevels=count};
    mtl_reg_tex(tid, &t, &v);
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> view = tex_by_id(tid);
    CHECK((view != nil) == (expected != 0), "invalid start is empty; valid start creates view");
    if (expected) {
        CHECK(view.mipmapLevelCount == expected, "effective mip count");
        CHECK(view.width == (64u >> level), "view retains requested base level");
        CHECK(view.arrayLength == layers, "array layers preserved");
        CHECK(tex_by_id(tid) == view, "cached view preserves range");
    }
    os_unfair_lock_unlock(&g_rmu);
}
static MTLTextureDescriptor *desc_of(int target, unsigned w, unsigned h, unsigned depth, unsigned levels, unsigned samples) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=98000};
    t.d = (nvn_texdesc){.target=target, .format=NVN_FORMAT_RGBA8, .width=w, .height=h, .depth=depth, .levels=levels, .samples=samples};
    return tex_desc(&t, 0);
}
static void desc_caps(void) {
    MTLTextureDescriptor *d = desc_of(NVN_TEXTURE_TARGET_2D, 1, 1, 1, 9, 1);   // a mod's 1x1 texture declaring 9 levels: Metal aborted the process
    CHECK(d && d.mipmapLevelCount == 1, "1x1 with 9 levels: one level");
    id<MTLTexture> one = d ? [g_dev newTextureWithDescriptor:d] : nil;
    CHECK(one != nil, "1x1 with 9 levels: made");
    CHECK(one && tex_level_ok(one, 0, "test") && !tex_level_ok(one, 3, "test"), "a copy at a missing level is skipped");
    d = desc_of(NVN_TEXTURE_TARGET_2D, 64, 16, 1, 12, 1);
    CHECK(d && d.mipmapLevelCount == 7, "64x16 with 12 levels: seven");
    d = desc_of(NVN_TEXTURE_TARGET_3D, 4, 4, 16, 9, 1);
    CHECK(d && d.mipmapLevelCount == 5 && [g_dev newTextureWithDescriptor:d], "4x4x16 3D with 9 levels: five (the depth counts)");
    d = desc_of(NVN_TEXTURE_TARGET_2D, 64, 64, 1, 7, 1);
    CHECK(d && d.mipmapLevelCount == 7, "a full chain is kept");
    d = desc_of(NVN_TEXTURE_TARGET_2D_ARRAY, 8, 8, 0, 1, 1);
    CHECK(d && d.arrayLength == 1 && [g_dev newTextureWithDescriptor:d], "array of no layers: one");
    CHECK(desc_of(NVN_TEXTURE_TARGET_1D_ARRAY, 8, 4096, 1, 1, 1) == nil, "1D array of 4096 layers: not made");
    d = desc_of(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 16, 16, 1, 1, 8);
    CHECK([g_dev supportsTextureSampleCount:8] ? d && d.sampleCount == 8 && [g_dev newTextureWithDescriptor:d] : d == nil, "8 samples: made where the GPU has them, else not made");
    d = desc_of(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 16, 16, 1, 1, 4);
    CHECK(d && d.sampleCount == 4 && d.mipmapLevelCount == 1 && [g_dev newTextureWithDescriptor:d], "4 samples: made");
    CHECK(desc_of(NVN_TEXTURE_TARGET_2D_MULTISAMPLE, 16, 16, 1, 1, 1) == nil, "multisample of one sample: not made");
}
int main(void) { @autoreleasepool {
    mtl_init();
    desc_caps();
    view_case(300, 0, 7, 7, 1);
    view_case(301, 1, 7, 6, 6);
    view_case(302, 1, 2, 2, 1);
    view_case(303, 3, 0, 4, 1);
    view_case(304, 6, 7, 1, 1);
    view_case(305, 7, 1, 0, 1);
    view_case(306, 255, 0, 0, 1);
    printf("texture view levels: %d checks, %d failed\n", checks, fails);
    return fails != 0;
} }
