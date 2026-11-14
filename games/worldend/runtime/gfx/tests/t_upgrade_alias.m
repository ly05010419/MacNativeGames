// A usage upgrade replaces one shared image, including every serial naming it.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    tex_t a = {.magic=MAGIC('T','E','X',0), .serial=99301};
    a.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&a.d, &a.L); tex_t b = a, ghost = a;
    b.serial++; ghost.serial += 2;
    mtl_tex_init(&a); mtl_tex_init(&b);
    id<MTLTexture> old = tex_mtl(&a);
    CHECK(tex_mtl(&b) == old && tex_mtl(&ghost) == old, "live and finalized serials initially share storage");
    // Exercise replacement at the hash table's growth threshold: reinserting while iterating would resize and reshuffle it.
    uint64_t extra = 99400;
    while ((g_by_serial.n + 1) * 4 <= g_by_serial.cap * 3) { hm_put_id(&g_by_serial, &extra, old); extra++; }
    uint32_t capacity = g_by_serial.cap, count = g_by_serial.n;
    id<MTLTexture> upgraded = tex_upgrade_pfv(&a, old, MTLPixelFormatR32Uint);
    CHECK(upgraded && upgraded != old, "usage upgrade creates a replacement");
    CHECK(g_by_serial.cap == capacity && g_by_serial.n == count, "replacement does not grow or reshape the serial table");
    int all = 1; for (uint64_t serial = 99400; serial < extra; serial++) all &= tex_get(serial) == upgraded;
    CHECK(all, "every shared serial at the growth threshold sees the replacement");
    CHECK(tex_get(b.serial) == upgraded, "other live serial sees the replacement");
    CHECK(tex_get(ghost.serial) == upgraded, "finalized serial sees the replacement");
    CHECK(!alias_has(old) && alias_has(upgraded), "sharing membership follows the replacement");
    uint32_t pixels[16]; for (int i = 0; i < 16; i++) pixels[i] = 0x44332211u;
    id<MTLBuffer> src = [g_dev newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared]; res_add(src);
    id<MTL4ComputeCommandEncoder> encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), upgraded, 0, 0, MTLOriginMake(0,0,0));
    for (int j = 0; j < 2; j++) {
        id<MTLTexture> texture = tex_mtl(j ? &ghost : &b);
        id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof pixels options:MTLResourceStorageModeShared]; res_add(dst);
        encoder = blit(); enc_b_copy_texture_buffer(encoder, texture, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
        mtl_sub_commit(); mtl_drain();
        CHECK(!memcmp(dst.contents, pixels, sizeof pixels), "other serial reads every GPU-written pixel after upgrade");
        if (!j) mtl_sub_begin();
    }
    CHECK(![g_res_objects containsObject:old], "retired base is removed from the residency set after GPU completion");
    printf("texture upgrade aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
