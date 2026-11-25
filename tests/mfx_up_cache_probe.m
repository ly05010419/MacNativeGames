/* The MetalFX scaler cache (gfx/nvn/totk_mfx_up.m) with a real Metal device: two configurations that take turns every
   present must both become usable, even when the cache is already full of older ones and the scaler in use is one of
   those. That is the window stage with frame interpolation on: the game's frames and the interpolated ones reach it from
   two textures of different sizes, every other present. A full cache whose newest entry was always the next one to be
   replaced built a scaler at every present and used none (Breath of the Wild, 2026-11-23: 1080p + MetalFX to 2560x1440
   with 60+60 after several picks; "built in the background" about once a frame, no "switched to").
   usage: mfx_up_cache_probe; exit 77 (skip) on a Mac without a Metal 4 MetalFX scaler, such as a CI runner's virtual GPU */
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "totk_mfx_up.h"

static id<MTLDevice> dev;
static id<MTLTexture> texture(NSUInteger w, NSUInteger h) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [dev newTextureWithDescriptor:d]; assert(t); return t;
}
static void pause_ms(int ms) { struct timespec t = {0, ms * 1000000L}; nanosleep(&t, NULL); }
/* Asks until the scaler for src -> 256x256 is the current one (it is built in the background meanwhile). */
static void make_current(id<MTLTexture> src) {
    for (int i = 0; i < 2000; i++) { if (mfxup_prepare(MFXUP_OUTPUT, src, nil, 256, 256, 0)) return; pause_ms(2); }
    assert(!"a scaler was never built");
}

int main(void) {
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        /* The same requirements as the scaler cache itself (shared_objects): without them it never builds a scaler. */
        if (!dev || ![dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:nil] ||
            ![MTLFXSpatialScalerDescriptor supportsMetal4FX:dev]) { printf("skip: no Metal 4 MetalFX scaler on this Mac\n"); return 77; }
        /* Three older configurations fill the cache; the last one stays current and is not asked for again. */
        id<MTLTexture> old[3] = {texture(64, 64), texture(80, 80), texture(96, 96)};
        for (int i = 0; i < 3; i++) make_current(old[i]);
        /* Two new ones in turn, a present apart (a frame at 60 fps is about 16 ms; the builds take a few ms). */
        id<MTLTexture> game = texture(112, 112), interpolated = texture(128, 128);
        int ready[2] = {0, 0};
        for (int i = 0; i < 200; i++) {
            ready[i & 1] += mfxup_prepare(MFXUP_OUTPUT, i & 1 ? interpolated : game, nil, 256, 256, 0);
            pause_ms(4);
        }
        printf("game frames ready %d / 100, interpolated frames ready %d / 100\n", ready[0], ready[1]);
        /* Both are built once and then kept: only the presents before their builds finish fall back. */
        assert(ready[0] >= 90 && ready[1] >= 90);
        printf("pass: two configurations in turn are kept\n");
    }
    return 0;
}
