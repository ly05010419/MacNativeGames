// Registered texture views use only the mip levels present in the texture; exercise the production Metal path.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
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
    pthread_mutex_lock(&g_rmu);
    id<MTLTexture> view = tex_by_id(tid);
    CHECK((view != nil) == (expected != 0), "invalid start is empty; valid start creates view");
    if (expected) {
        CHECK(view.mipmapLevelCount == expected, "effective mip count");
        CHECK(view.width == (64u >> level), "view retains requested base level");
        CHECK(view.arrayLength == layers, "array layers preserved");
        CHECK(tex_by_id(tid) == view, "cached view preserves range");
    }
    pthread_mutex_unlock(&g_rmu);
}
int main(void) { @autoreleasepool {
    mtl_init();
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
