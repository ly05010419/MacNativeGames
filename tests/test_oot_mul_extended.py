"""Extended products leave SPIRV-Cross's optnone helper (totk_xlate Service.cs MulExtended, after nxnative 37f076a5): scalar
uint / int calls of spvMulExtended reach inline overloads that take the halves of the 64-bit product. On the GPU, the
overloads give the same two words as the optnone template (mulhi) for 65,536 operand pairs each, edge values included;
vector calls keep the template. No game files."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SERVICE = (ROOT/'games/oot/tools/totk_xlate/Service.cs').read_text(encoding='utf-8')
TEMPLATE = ('template<typename T, typename U, typename V>\n[[clang::optnone]] T spvMulExtended(V l, V r)\n{\n'
            '    return T{U(l * r), U(mulhi(l, r))};\n}\n')


def overloads():
    """The overloads Service.cs writes, from its own format string."""
    fmt = re.search(r'scalar \+= \$"(.*?)\\n";', SERVICE).group(1)
    out = ''
    for t, w in (('uint', 'ulong'), ('int', 'long')):
        out += fmt.replace('{t}', t).replace('{w}', w).replace('{{', '{').replace('}}', '}') + '\n'
    return out


HARNESS = r'''
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
enum { N = 65536 };
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) { puts("SKIP"); return 0; }
    NSError *e = nil; NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&e];
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;
    id<MTLLibrary> lib = [d newLibraryWithSource:src options:o error:&e]; if (!lib) { fprintf(stderr, "%s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&e]; if (!ps) return 1;
    id<MTLBuffer> in = [d newBufferWithLength:N * 8 options:MTLResourceStorageModeShared], out = [d newBufferWithLength:N * 32 options:MTLResourceStorageModeShared];
    uint32_t *v = in.contents; srand(7);
    const uint32_t edge[] = {0, 1, 2, 3, 0x7fffffffu, 0x80000000u, 0x80000001u, 0xfffffffeu, 0xffffffffu, 0x10000u, 0xffffu, 1000000007u};
    for (int i = 0; i < N * 2; i++) v[i] = i < 144 ? edge[(i / 2) % 12 + 0] ^ (i & 1 ? edge[(i / 24) % 12] : 0) : ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:ps]; [c setBuffer:in offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
    [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [c endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint32_t *r = out.contents; int bad = 0;
    for (int i = 0; i < N; i++) {
        uint64_t up = (uint64_t)v[2 * i] * v[2 * i + 1]; int64_t sp = (int64_t)(int32_t)v[2 * i] * (int32_t)v[2 * i + 1];
        uint32_t want[8] = {(uint32_t)up, (uint32_t)(up >> 32), (uint32_t)up, (uint32_t)(up >> 32), (uint32_t)sp, (uint32_t)((uint64_t)sp >> 32), (uint32_t)sp, (uint32_t)((uint64_t)sp >> 32)};
        for (int k = 0; k < 8; k++) if (r[8 * i + k] != want[k] && bad++ < 4) fprintf(stderr, "pair %d word %d: %08x want %08x\n", i, k, r[8 * i + k], want[k]);
    }
    if (!bad) puts("mul extended passed");
    return bad != 0;
} }
'''

KERNEL = r'''
struct ResU { uint lo; uint hi; };
struct ResS { int lo; int hi; };
kernel void k(device const uint2* in [[buffer(0)]], device uint* out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    uint a = in[i].x, b = in[i].y;
    ResU u = spvMulExtended<ResU, uint>(a, b);                          // the scalar overload
    uint2 ul = uint2(a, 0u), ur = uint2(b, 0u);
    struct ResU2 { uint2 lo; uint2 hi; } uv = spvMulExtended<ResU2, uint2>(ul, ur);   // a vector keeps the template
    ResS s = spvMulExtended<ResS, int>(int(a), int(b));
    struct ResS2 { int2 lo; int2 hi; } sv = spvMulExtended<ResS2, int2>(int2(int(a), 0), int2(int(b), 0));
    out[8 * i + 0] = u.lo; out[8 * i + 1] = u.hi; out[8 * i + 2] = uv.lo.x; out[8 * i + 3] = uv.hi.x;
    out[8 * i + 4] = uint(s.lo); out[8 * i + 5] = uint(s.hi); out[8 * i + 6] = uint(sv.lo.x); out[8 * i + 7] = uint(sv.hi.x);
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'Metal')
class MulExtended(unittest.TestCase):
    def test_overloads_match_the_template_on_the_gpu(self):
        source = '#include <metal_stdlib>\nusing namespace metal;\n' + TEMPLATE + overloads() + KERNEL
        self.assertIn('always_inline', source)
        self.assertNotIn('optnone]] T spvMulExtended(uint', source)
        with tempfile.TemporaryDirectory() as tmp:
            msl, harness, binary = Path(tmp)/'k.metal', Path(tmp)/'h.m', Path(tmp)/'h'
            msl.write_text(source); harness.write_text(HARNESS)
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', str(harness), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binary), str(msl)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue('mul extended passed' in run.stdout or 'SKIP' in run.stdout, run.stdout)

    def test_the_post_process_applies_it_to_every_stage(self):
        self.assertIn('MulExtended(PoolArrays(PreciseMinMax(MatchSwitchDepthSampling(src))))', SERVICE)
        self.assertIn(r'\[\[clang::optnone\]\] T spvMulExtended\(V l, V r\)', SERVICE)   # it finds the template SPIRV-Cross writes


if __name__ == '__main__':
    unittest.main()
