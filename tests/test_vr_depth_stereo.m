// zeldaVR depth stereo (zeldaVR/gfx/vr_depth_stereo.m) on the GPU: one picture and its depth through the real capture
// ring and eye passes.
//  - Upper half: a 40 px object with a disparity of exactly 6 px in front of a background at infinity lands 6 px to the
//    right in the left eye and 6 px to the left in the right eye, 40 px wide in both, with no copy of it left in the
//    background it uncovered (2026-11-22, the Quest: "人物拉伸": a hole took the object's own pixel).
//  - Lower half: a nearer object (20 px, over the cap of 64 px at 1080 lines = 7.6 px here) keeps its width and moves by
//    the cap (a cap on the search alone cut such an object to pieces).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zeldaVR/gfx/vr_depth_stereo.h"

static id<MTLResidencySet> g_rs;
id<MTLResidencySet> vr_residency(void) { return g_rs; }
void vr_residency_swap(id<MTLAllocation> old, id<MTLAllocation> replacement) { (void)old; (void)replacement; }
int vr_dstereo(void) { return 1; }

/* focal = H / 2 / tan 1 = 64 px; k = focal * half / near = 64 * 0.1 / 0.1 = 64: disparity = 64 * (1 - d) */
enum { W = 256, H = 128, OBJ0 = 100, OBJW = 40, SHIFT = 6, NEAR_SHIFT = 20 };

static int fail(const char *m) { fprintf(stderr, "depth stereo: %s\n", m); return 1; }

int main(void) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) return fail("no Metal device");
        g_rs = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:nil];
        id<MTLCommandQueue> q = [dev newCommandQueue];
        // the picture: red = column, green = 255 on the object; the depth: the object nearer in the lower half
        MTLTextureDescriptor *pd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        pd.usage = MTLTextureUsageShaderRead; pd.storageMode = MTLStorageModeShared;
        id<MTLTexture> pic = [dev newTextureWithDescriptor:pd];
        uint8_t *px = calloc(W * H, 4); float *dp = calloc(W * H, 4);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            const int obj = x >= OBJ0 && x < OBJ0 + OBJW;
            uint8_t *p = px + 4 * (y * W + x); p[0] = (uint8_t)x; p[1] = obj ? 255 : 0; p[2] = 0; p[3] = 255;
            dp[y * W + x] = obj ? 1.0f - (y < H / 2 ? SHIFT : NEAR_SHIFT) / 64.0f : 1.0f;
        }
        [pic replaceRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0 withBytes:px bytesPerRow:W * 4];
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
        dd.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];
        id<MTLBuffer> db = [dev newBufferWithBytes:dp length:W * H * 4 options:MTLResourceStorageModeShared];
        // the game's depth, then the capture at the present (the copy the runtime encodes on its own queue)
        vr_ds_note_scene(depth, 0.1f, W, H);
        id<MTLTexture> from = nil, to = vr_ds_capture_target(&from);
        if (!to || from != depth) return fail("no ring slot for the noted depth");
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromBuffer:db sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:W * H * 4 sourceSize:MTLSizeMake(W, H, 1) toTexture:depth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b copyFromTexture:from toTexture:to];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        vr_ds_captured();
        const uint32_t ref = vr_ds_take_captured();
        if (!ref) return fail("the capture gave no reference");
        if (vr_ds_take_captured()) return fail("a reference was handed out twice");
        // the eyes: tan 1 (focal 64 px), the 3D view the whole width (aspect 2), half the eye distance 0.1
        __strong id<MTLTexture> eyes[2] = { nil, nil };
        cb = [q commandBuffer];
        if (!vr_ds_eyes(cb, pic, 1, ref, 1.0f, 2.0f, 0.1f, eyes) || !eyes[0] || !eyes[1]) return fail("no eye pictures");
        id<MTLBuffer> rb[2];
        b = [cb blitCommandEncoder];
        for (int k = 0; k < 2; k++) {
            rb[k] = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
            [b copyFromTexture:eyes[k] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                      toBuffer:rb[k] destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
        }
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) return fail(cb.error.localizedDescription.UTF8String);
        const float cap = 64.0f * H / 1080.0f;
        int bad = 0;
        for (int k = 0; k < 2; k++) {
            const uint8_t *e = rb[k].contents; const int dir = k == 0 ? 1 : -1;
            for (int y = 8; y < H - 8; y += 8) {
                if (y >= H / 2 - 4 && y < H / 2 + 4) continue;   // the rows where the two objects meet
                int first = -1, last = -1, n = 0;
                for (int x = 0; x < W; x++) if (e[4 * (y * W + x) + 1] > 128) { if (first < 0) first = x; last = x; n++; }
                const int exact = y < H / 2;
                const float want = OBJ0 + dir * (exact ? (float)SHIFT : cap);
                /* exact: the object 40 px wide at want, nothing else green; capped: a fraction of a pixel either way */
                const int ok = exact ? first == (int)want && n == OBJW && last == first + OBJW - 1
                                     : n >= OBJW - 1 && n <= OBJW + 1 && last - first + 1 == n && fabsf((float)first - want) <= 1.0f;
                if (!ok) { fprintf(stderr, "depth stereo: %s eye row %d: the object %d..%d (%d px green), want from %.1f, %d wide\n", k ? "right" : "left", y, first, last, n, want, OBJW); bad++; }
                // the background far from the object stays where it is (zero disparity at infinity)
                for (int x = 0; x < 60; x++) if (e[4 * (y * W + x)] != x) { fprintf(stderr, "depth stereo: %s eye row %d column %d moved (%d)\n", k ? "right" : "left", y, x, e[4 * (y * W + x)]); bad++; break; }
            }
        }
        // a second call for the same picture sequence reuses the pictures (made once per new game picture)
        __strong id<MTLTexture> again[2] = { nil, nil };
        cb = [q commandBuffer];
        if (!vr_ds_eyes(cb, pic, 1, ref, 1.0f, 2.0f, 0.1f, again) || again[0] != eyes[0] || again[1] != eyes[1]) { fprintf(stderr, "depth stereo: not reused for the same sequence\n"); bad++; }
        [cb commit];
        free(px); free(dp);
        if (bad) return 1;
        printf("depth stereo passed: the object 6 px apart in each eye and 40 px wide, a nearer one moved by the cap and whole, no copy in the uncovered background\n");
    }
    return 0;
}
