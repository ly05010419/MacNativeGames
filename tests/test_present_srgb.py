"""The present shows the bytes stored in the swapchain, as the console's scan-out does. Pokopia's swapchain is NVN RGBA8_SRGB
(0x38): sampled as sRGB it decodes to linear, the BGRA8Unorm layer does not encode again, and the whole picture comes out dark
(measured: stored mean 144, shown 88, the sRGB decode of the stored bytes 86). The real present_raw_src, on a real Metal device."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'

PROBE = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <pthread.h>
#include <stdio.h>
static int g_released;
static void defer_obj_unref(id obj) { (void)obj; g_released++; }
#define RED_ONCE(k, ...) fprintf(stderr, __VA_ARGS__)
%s
static int read_value(id<MTLDevice> d, id<MTLCommandQueue> q, id<MTLTexture> t) {
    NSError *e = nil;
    id<MTLLibrary> l = [d newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void k(texture2d<float> t [[texture(0)]], device float *o [[buffer(0)]]) { o[0] = t.read(uint2(0, 0)).r; }" options:nil error:&e];
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e];
    id<MTLBuffer> b = [d newBufferWithLength:16 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [c computeCommandEncoder];
    [en setComputePipelineState:p]; [en setTexture:t atIndex:0]; [en setBuffer:b offset:0 atIndex:0];
    [en dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [en endEncoding];
    [c commit]; [c waitUntilCompleted];
    return (int)(((float *)b.contents)[0] * 255 + 0.5f);
}
static id<MTLTexture> tex(id<MTLDevice> d, MTLPixelFormat f, uint8_t v) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [d newTextureWithDescriptor:td]; uint8_t px[4] = {v, v, v, 255};
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4]; return t;
}
int main(void) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) { puts("no-device"); return 0; }
    id<MTLCommandQueue> q = [d newCommandQueue];
    id<MTLTexture> srgb = tex(d, MTLPixelFormatRGBA8Unorm_sRGB, 144), bgra = tex(d, MTLPixelFormatBGRA8Unorm_sRGB, 144);
    id<MTLTexture> plain = tex(d, MTLPixelFormatRGBA8Unorm, 144), hdr = tex(d, MTLPixelFormatRGBA8Snorm, 40);
    id<MTLTexture> v = present_raw_src(srgb), w = present_raw_src(bgra);
    printf("decoded=%%d raw=%%d bgra=%%d\n", read_value(d, q, srgb), read_value(d, q, v), read_value(d, q, w));
    printf("formats=%%d,%%d same_storage=%%d\n", v.pixelFormat == MTLPixelFormatRGBA8Unorm, w.pixelFormat == MTLPixelFormatBGRA8Unorm, v.parentTexture == srgb);
    printf("cached=%%d passthrough=%%d,%%d nil=%%d\n", present_raw_src(srgb) == v, present_raw_src(plain) == plain, present_raw_src(hdr) == hdr, present_raw_src(nil) == nil);
    for (int i = 0; i < 6; i++) present_raw_src(tex(d, MTLPixelFormatRGBA8Unorm_sRGB, 1));
    printf("replaced_views_deferred=%%d\n", g_released);
} return 0; }
'''


def function_text():
    text = SOURCE.read_text(errors='replace')
    start = text.index('static id<MTLTexture> present_raw_src(id<MTLTexture> src) {')
    return text[start:text.index('\n}\n', start) + 3]


class PresentPaths(unittest.TestCase):
    def body(self, name, next_name):
        text = SOURCE.read_text(errors='replace')
        start = text.index(f'static int {name}(', text.index(f'static int {name}(') + 1)
        return text[start:text.index(next_name, start)]

    def test_the_render_path_samples_the_raw_view(self):
        body = self.body('present_encode', 'static int present_encode_m4(')
        self.assertIn('present_raw_src(src)', body)
        self.assertNotIn('setFragmentTexture:src atIndex:0', body)

    def test_the_metal_4_path_takes_it_before_metalfx(self):
        body = self.body('present_encode_m4', 'MTL4RenderPassDescriptor *rp')
        raw, upscale = body.index('present_raw_src(src)'), body.index('output_upscale(cb, raw,')
        self.assertLess(raw, upscale)
        self.assertRegex(body, r'id<MTLTexture> pic = up \? up : raw;')


@unittest.skipUnless(sys.platform == 'darwin', 'Metal')
class RealMetal(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.dir.cleanup)
        source, binary = Path(cls.dir.name) / 'probe.m', Path(cls.dir.name) / 'probe'
        source.write_text(PROBE % function_text())
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Werror', '-framework', 'Foundation', '-framework', 'Metal', str(source),
                        '-o', str(binary)], check=True, capture_output=True, text=True)
        cls.out = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=60).stdout

    def value(self, name):
        return int(re.search(rf'{name}=(-?\d+)', self.out).group(1))

    def test_the_view_reads_the_stored_bytes(self):
        if 'no-device' in self.out:
            self.skipTest('no Metal device')
        self.assertLess(self.value('decoded'), 90)   # what the screen showed: 144 decoded to linear
        self.assertEqual((self.value('raw'), self.value('bgra')), (144, 144))

    def test_it_is_a_view_of_the_same_storage(self):
        if 'no-device' in self.out:
            self.skipTest('no Metal device')
        self.assertIn('formats=1,1 same_storage=1', self.out)

    def test_other_formats_are_returned_as_they_are(self):
        if 'no-device' in self.out:
            self.skipTest('no Metal device')
        self.assertIn('cached=1 passthrough=1,1 nil=1', self.out)

    def test_a_replaced_view_outlives_its_submissions(self):
        if 'no-device' in self.out:
            self.skipTest('no Metal device')
        self.assertGreaterEqual(self.value('replaced_views_deferred'), 4)


if __name__ == '__main__':
    unittest.main()
