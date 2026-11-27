"""A picture larger than the screen (2K, 4K or 5K into a smaller window, a 1080p TV or a Retina panel) is supersampling:
the player pays for the larger render to get smoother edges. The present must then average every texel a screen pixel
covers, in linear light. One bilinear tap per screen pixel uses all four texels only at exactly 2x, and averages the
stored (gamma) values: at 1.27x or 1.5x part of the render never reaches the screen, and thin bright edges darken.
A picture at or below the screen size, and the overlay boxes drawn with the same pipeline, must present exactly as
before. The real present shader source (totk_hdr_present.h), on a real Metal device."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr_present.h'

PROBE = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "@HEADER@"

static id<MTLDevice> D; static id<MTLCommandQueue> Q; static id<MTLSamplerState> S;
static uint32_t rng = 12345;
static uint8_t rnd(void) { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); }
static double lin(double v) { return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4); }

static id<MTLRenderPipelineState> pipeline(MTLPixelFormat dst, BOOL hdr, BOOL box, NSString *fn) {
    if (box) unsetenv("TOTK_PRESENT_BOX"); else setenv("TOTK_PRESENT_BOX", "0", 1);
    NSError *e = nil;
    id<MTLLibrary> l = [D newLibraryWithSource:totk_hdr_present_source(dst, hdr) options:nil error:&e];
    if (!l) { fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String); exit(2); }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [l newFunctionWithName:@"vs"]; pd.fragmentFunction = [l newFunctionWithName:fn];
    pd.colorAttachments[0].pixelFormat = dst;
    id<MTLRenderPipelineState> p = [D newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!p) { fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String); exit(2); }
    unsetenv("TOTK_PRESENT_BOX");
    return p;
}
/* An RGBA source whose texel (x, y) is f(x, y) in every colour channel, alpha 255. */
static id<MTLTexture> source8(int w, int h, uint8_t (^f)(int, int)) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [D newTextureWithDescriptor:td];
    uint8_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t v = f(x, y); uint8_t *p = px + ((size_t)y * w + x) * 4; p[0] = p[1] = p[2] = v; p[3] = 255; }
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:(NSUInteger)w * 4];
    free(px); return t;
}
static id<MTLTexture> target(int w, int h, MTLPixelFormat f) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    return [D newTextureWithDescriptor:td];
}
/* present_encode's constants for the valid rectangle cx, cy, cw, ch of src (HDRCrop: headroom 1, UI white 1). */
static void crop_consts(id<MTLTexture> src, int cx, int cy, int cw, int ch, float *uv) {
    float W = (float)src.width, H = (float)src.height;
    float v[10] = {cx / W, cy / H, cw / W, ch / H, (cx + 0.5f) / W, (cy + 0.5f) / H, (cx + cw - 0.5f) / W, (cy + ch - 0.5f) / H, 1, 1};
    memcpy(uv, v, sizeof v);
}
static double draw(id<MTLRenderPipelineState> p, id<MTLTexture> src, id<MTLTexture> dst, const float *uv) {
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [Q commandBuffer]; id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:(MTLViewport){0, 0, (double)dst.width, (double)dst.height, 0, 1}];
    [en setRenderPipelineState:p]; [en setFragmentTexture:src atIndex:0]; [en setFragmentSamplerState:S atIndex:0];
    [en setFragmentBytes:uv length:10 * sizeof(float) atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    return (cb.GPUEndTime - cb.GPUStartTime) * 1000;
}
/* The red channel of a BGRA8 target, one byte per pixel. */
static uint8_t *red8(id<MTLTexture> t) {
    size_t n = t.width * t.height; uint8_t *bgra = malloc(n * 4), *r = malloc(n);
    [t getBytes:bgra bytesPerRow:t.width * 4 fromRegion:MTLRegionMake2D(0, 0, t.width, t.height) mipmapLevel:0];
    for (size_t i = 0; i < n; i++) r[i] = bgra[i * 4 + 2];
    free(bgra); return r;
}
static uint8_t *present8(BOOL box, id<MTLTexture> src, int cx, int cy, int cw, int ch, int dw, int dh) {
    static id<MTLRenderPipelineState> ps[2];
    if (!ps[box]) ps[box] = pipeline(MTLPixelFormatBGRA8Unorm, NO, box, @"fs");
    id<MTLTexture> dst = target(dw, dh, MTLPixelFormatBGRA8Unorm); float uv[10]; crop_consts(src, cx, cy, cw, ch, uv);
    draw(ps[box], src, dst, uv); return red8(dst);
}
static void minmax(const char *name, const uint8_t *r, size_t n) {
    int lo = 255, hi = 0; double sum = 0;
    for (size_t i = 0; i < n; i++) { if (r[i] < lo) lo = r[i]; if (r[i] > hi) hi = r[i]; sum += r[i]; }
    printf("%s_min=%d\n%s_max=%d\n%s_mean=%.3f\n", name, lo, name, hi, name, sum / n);
}
static void lin_mean(const char *name, const uint8_t *r, size_t n) {
    double sum = 0; for (size_t i = 0; i < n; i++) sum += lin(r[i] / 255.0);
    printf("%s=%.5f\n", name, sum / n);
}
static size_t diff(const uint8_t *a, const uint8_t *b, size_t n) { size_t d = 0; for (size_t i = 0; i < n; i++) d += a[i] != b[i]; return d; }

int main(void) { @autoreleasepool {
    D = MTLCreateSystemDefaultDevice(); if (!D) { puts("no-device"); return 0; }
    Q = [D newCommandQueue];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; S = [D newSamplerStateWithDescriptor:sd];
    uint8_t (^checker)(int, int) = ^uint8_t(int x, int y) { return ((x + y) & 1) ? 255 : 0; };
    uint8_t (^stripes)(int, int) = ^uint8_t(int x, int y) { (void)y; return x % 3 == 0 ? 255 : 0; };

    id<MTLTexture> c4k = source8(3840, 2160, checker);
    uint8_t *n2 = present8(YES, c4k, 0, 0, 3840, 2160, 1920, 1080), *o2 = present8(NO, c4k, 0, 0, 3840, 2160, 1920, 1080);
    minmax("new_2x", n2, 1920 * 1080); minmax("old_2x", o2, 1920 * 1080);

    id<MTLTexture> s4k = source8(3840, 2160, stripes);
    lin_mean("new_lin_mean_150", present8(YES, s4k, 0, 0, 3840, 2160, 2560, 1440), 2560 * 1440);
    lin_mean("old_lin_mean_150", present8(NO, s4k, 0, 0, 3840, 2160, 2560, 1440), 2560 * 1440);
    lin_mean("new_lin_mean_127", present8(YES, s4k, 0, 0, 3840, 2160, 3024, 1701), 3024 * 1701);

    id<MTLTexture> noise = source8(1920, 1080, ^uint8_t(int x, int y) { (void)x; (void)y; return rnd(); });
    printf("diff_1to1=%zu\n", diff(present8(YES, noise, 0, 0, 1920, 1080, 1920, 1080), present8(NO, noise, 0, 0, 1920, 1080, 1920, 1080), 1920 * 1080));
    printf("diff_up=%zu\n", diff(present8(YES, noise, 0, 0, 1920, 1080, 2560, 1440), present8(NO, noise, 0, 0, 1920, 1080, 2560, 1440), 2560 * 1440));

    id<MTLTexture> boxed = source8(4000, 2300, ^uint8_t(int x, int y) { return x < 3840 && y < 2160 ? 255 : 0; });
    uint8_t *cr = present8(YES, boxed, 0, 0, 3840, 2160, 2560, 1440); minmax("crop", cr, 2560 * 1440);

    id<MTLTexture> c5k = source8(5120, 2880, checker);
    minmax("far", present8(YES, c5k, 0, 0, 5120, 2880, 1000, 562), 1000 * 562);

    {   // SDR picture into an EDR (RGBA16Float) drawable: the same averaging, returned linear
        id<MTLRenderPipelineState> p = pipeline(MTLPixelFormatRGBA16Float, YES, YES, @"fs");
        id<MTLTexture> dst = target(1920, 1080, MTLPixelFormatRGBA16Float); float uv[10]; crop_consts(c4k, 0, 0, 3840, 2160, uv);
        draw(p, c4k, dst, uv);
        size_t n = 1920 * 1080; __fp16 *h = malloc(n * 8); double sum = 0, lo = 1e9, hi = -1e9;
        [dst getBytes:h bytesPerRow:1920 * 8 fromRegion:MTLRegionMake2D(0, 0, 1920, 1080) mipmapLevel:0];
        for (size_t i = 0; i < n; i++) { double v = h[i * 4]; sum += v; if (v < lo) lo = v; if (v > hi) hi = v; }
        printf("edr_mean=%.5f\nedr_spread=%.5f\n", sum / n, hi - lo); free(h);
    }
    for (int box = 0; box < 2; box++) {
        id<MTLRenderPipelineState> p = pipeline(MTLPixelFormatBGRA8Unorm, NO, box, @"fs");
        id<MTLTexture> dst = target(2560, 1440, MTLPixelFormatBGRA8Unorm); float uv[10]; crop_consts(c4k, 0, 0, 3840, 2160, uv);
        double ms[20]; for (int i = 0; i < 20; i++) ms[i] = draw(p, c4k, dst, uv);
        for (int i = 0; i < 20; i++) for (int j = i + 1; j < 20; j++) if (ms[j] < ms[i]) { double t = ms[i]; ms[i] = ms[j]; ms[j] = t; }
        printf("gpu_ms_%s=%.3f\n", box ? "new" : "old", ms[10]);
    }
} return 0; }
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Metal')
class PresentDownscale(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.dir.cleanup)
        source, binary = Path(cls.dir.name) / 'probe.m', Path(cls.dir.name) / 'probe'
        source.write_text(PROBE.replace('@HEADER@', str(HEADER)))
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Werror', '-Wno-unused-function', '-framework', 'Foundation',
                        '-framework', 'Metal', str(source), '-o', str(binary)], check=True, capture_output=True, text=True)
        cls.out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=300).stdout
        print('\n' + ' '.join(l for l in cls.out.splitlines() if l.startswith('gpu_ms_')), file=sys.stderr)

    def setUp(self):
        if 'no-device' in self.out:
            self.skipTest('no Metal device')

    def value(self, name):
        return int(re.search(rf'^{name}=(-?\d+)$', self.out, re.M).group(1))

    def fvalue(self, name):
        return float(re.search(rf'^{name}=(-?[\d.]+)$', self.out, re.M).group(1))

    def test_two_times_averages_in_linear_light(self):
        """1-pixel checker of 0 and 255, 3840x2160 into 1920x1080: half the light is white, so every pixel shows
        sRGB(0.5) = 188. The old tap averages the stored values (about 128), which darkens thin bright edges such as
        hair against the sky."""
        self.assertTrue(187 <= self.value('new_2x_min') and self.value('new_2x_max') <= 189, self.out)
        self.assertTrue(126 <= self.fvalue('old_2x_mean') <= 130, self.out)

    def test_every_texel_counts_at_fractional_ratios(self):
        """Vertical stripes 255, 0, 0 (a third of the light) at 1.5x (4K into 1440p) and 1.27x (4K into a Retina
        panel's 3024 wide): the output keeps a third of the light within 1 %. The old tap skips texels (about 0.26 at
        1.5x), so part of the render the player paid for never reached the screen."""
        for r in ('150', '127'):
            self.assertAlmostEqual(self.fvalue(f'new_lin_mean_{r}'), 1 / 3, delta=0.0034, msg=r)
        self.assertLess(self.fvalue('old_lin_mean_150'), 0.30)

    def test_one_to_one_and_upscale_are_unchanged(self):
        """A picture at or below the screen size (and the overlay boxes, drawn with the same pipeline) presents byte
        for byte as with the filter off."""
        self.assertEqual(self.value('diff_1to1'), 0)
        self.assertEqual(self.value('diff_up'), 0)

    def test_no_black_from_outside_the_crop(self):
        """Only the valid rectangle is the game's picture: its black surroundings must not creep in along the right
        and bottom edges."""
        self.assertEqual(self.value('crop_min'), 255)

    def test_beyond_four_times_stays_bounded(self):
        """5K into 1000x562 (5.12x, a phone-sized drawable): a finite, even picture rather than black, NaN or a
        stall."""
        self.assertTrue(180 <= self.value('far_min') and self.value('far_max') <= 196, self.out)


    def test_edr_present_averages_in_linear_light(self):
        """With HDR on, the SDR picture goes to an RGBA16Float drawable decoded to linear: at 2x the checker must be
        0.5 everywhere, as on an SDR screen, not the decode of the gamma average (about 0.21)."""
        self.assertAlmostEqual(self.fvalue('edr_mean'), 0.5, delta=0.01, msg=self.out)
        self.assertLess(self.fvalue('edr_spread'), 0.01)

if __name__ == '__main__':
    unittest.main()
