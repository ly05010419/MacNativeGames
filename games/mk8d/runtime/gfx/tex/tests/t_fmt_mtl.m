// Format table verification by actual creation: for every NVN format 0x01–0x9C take the mapping and actually create a 2D texture (with component remapping) on the local MTLDevice, counting how many can be created.
// Criterion: every format with a non-zero mapping is created (newTextureWithDescriptor non-nil); also verify that every NVN format with sRGB in its name lands on a Metal _sRGB format.
#import <Metal/Metal.h>
#include <stdio.h>
#include "../nvn_fmt_mtl.h"
int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        printf("device %s\n", dev.name.UTF8String);
        int mapped = 0, ok = 0, exact = 0, sub = 0, none = 0, bad_srgb = 0;
        static const int srgb[] = {0x37, 0x38, 0x45, 0x46, 0x47, 0x48, 0x4E, 0x9B, 0x9C};
        for (int f = 0x01; f <= 0x9C; f++) {
            uint32_t m; uint8_t s[4]; int k = nvn_fmt_to_mtl(f, &m, s);
            if (!k) { none++; continue; }
            mapped++; k == 1 ? exact++ : sub++;
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)m width:120 height:120 mipmapped:NO];
            d.swizzle = MTLTextureSwizzleChannelsMake(s[0], s[1], s[2], s[3]);
            BOOL depth = m >= 250 && m <= 262;
            d.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead | (depth ? MTLTextureUsageRenderTarget : 0);
            if (depth) d.swizzle = MTLTextureSwizzleChannelsDefault;     // remapping is not allowed for depth formats
            id<MTLTexture> t = [dev newTextureWithDescriptor:d];
            if (t) ok++; else printf("cannot create NVN %#x → MTL %u\n", f, m);
        }
        for (unsigned i = 0; i < sizeof srgb / sizeof *srgb; i++) {
            uint32_t m; nvn_fmt_to_mtl(srgb[i], &m, NULL);
            if (!(m == 71 || m == 81 || m == 131 || m == 133 || m == 135 || m == 153)) { printf("sRGB did not land on _sRGB: %#x → %u\n", srgb[i], m); bad_srgb++; }
        }
        for (int f = 0x87; f <= 0x94; f++) { uint32_t m; nvn_fmt_to_mtl(f, &m, NULL); if (m < 186 || m > 200) { printf("ASTC sRGB did not land on _sRGB: %#x → %u\n", f, m); bad_srgb++; } }
        printf("mapped (nonzero): %d (exact %d, substitute %d), no match %d, total %d; actually created %d/%d; sRGB violations %d\n", mapped, exact, sub, none, 0x9C, ok, mapped, bad_srgb);
        return (ok == mapped && !bad_srgb) ? 0 : 1;
    }
}
