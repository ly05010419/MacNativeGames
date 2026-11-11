"""Ampere HMMA.16816 with an f16 accumulator through simdgroup_matrix (dmsh_build.py hmma_msl): the rewrite of the translator's helper dmsh_hmma16816_f16 as spirv-cross
writes it, its arguments and scratch array, the compile with the chain's Metal flags, and on this Mac's GPU the rewritten body against a CPU reference of PTX mma.m16n8k16
(f16 sources and accumulator) on random fragments, four SIMD-groups a threadgroup."""
import ast
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name == 'hmma_msl')
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', None) == 'HMMA_BODY' for t in n.targets))]
scope = {'re': re}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
hmma_msl = scope['hmma_msl']

HEAD = '''#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];

    thread T& operator [] (size_t pos) thread { return elements[pos]; }
    constexpr const thread T& operator [] (size_t pos) const thread { return elements[pos]; }
    threadgroup T& operator [] (size_t pos) threadgroup { return elements[pos]; }
    constexpr const threadgroup T& operator [] (size_t pos) const threadgroup { return elements[pos]; }
};
'''
# The helper as spirv-cross writes it (Donkey Kong Bananza's 7629d857a0a93833, cut down to its signature and a body of the same shape)
HELPER = '''
int dmsh_hmma16816_f16(thread const int& _42961, thread const int& _42962, thread const int& _42963, thread const int& _42964, thread const int& _42965, thread const int& _42966, thread const int& _42967, thread const int& _42968, thread const int& _42969, thread const int& _42970, thread int& _42971, threadgroup spvUnsafeArray<uint, 1024>& _34)
{
    uint _42974 = uint(int(uint(_42961) >> uint(2)));
    _34[_42962 + int(_42974)] = uint(_42963);
    if (_42974 > 3u)
    {
        _42971 = 0;
    }
    simdgroup_barrier(mem_flags::mem_threadgroup);
    _42971 = int(_34[_42962]);
    return int(_34[_42962 + 1]);
}
'''
KERNEL = '''
kernel void check(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], uint lane [[thread_index_in_simdgroup]], uint sg [[simdgroup_index_in_threadgroup]],
                  uint gid [[thread_position_in_grid]])
{
    threadgroup spvUnsafeArray<uint, 1024> scr;
    int l = int(lane), blk = int(sg * 256);
    int a0 = int(in[gid * 8 + 0]), a1 = int(in[gid * 8 + 1]), a2 = int(in[gid * 8 + 2]), a3 = int(in[gid * 8 + 3]);
    int b0 = int(in[gid * 8 + 4]), b1 = int(in[gid * 8 + 5]), c0 = int(in[gid * 8 + 6]), c1 = int(in[gid * 8 + 7]);
    int d1;
    int d0 = dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, d1, scr);
    out[gid * 2] = uint(d0); out[gid * 2 + 1] = uint(d1);
}
'''
# The GPU check: 64 threadgroups of 128 lanes, random halves in [-1, 1]; the reference sums in float and rounds once; the matrix hardware rounds the f16 accumulator as
# it goes, as Ampere's f16 accumulator does, so the tolerance is 0.01 + 0.4 % of the value
RUNNER = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static float h2f(uint16_t h) { __fp16 v; memcpy(&v, &h, 2); return (float)v; }
static uint16_t f2h(float f) { __fp16 v = (__fp16)f; uint16_t h; memcpy(&h, &v, 2); return h; }
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *e = nil;
        if (!dev) { printf("no Metal device\n"); return 2; }
        NSData *lib_data = [NSData dataWithContentsOfFile:@(argv[1])];
        id<MTLLibrary> lib = [dev newLibraryWithData:dispatch_data_create(lib_data.bytes, lib_data.length, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT) error:&e];
        if (!lib) { printf("library: %s\n", e.localizedDescription.UTF8String); return 1; }
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"check"] error:&e];
        if (!ps) { printf("pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
        const int groups = 64, tg = 128, n = groups * tg;
        id<MTLBuffer> in = [dev newBufferWithLength:n * 32 options:MTLResourceStorageModeShared], out = [dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
        uint32_t *w = in.contents; srand(11);
        for (int i = 0; i < n * 8; i++) w[i] = f2h((rand() % 2001 - 1000) / 1000.0f) | (uint32_t)f2h((rand() % 2001 - 1000) / 1000.0f) << 16;
        id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:in offset:0 atIndex:0]; [ce setBuffer:out offset:0 atIndex:1];
        [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t *o = out.contents; long bad = 0; double worst = 0;
        for (int warp = 0; warp < n / 32; warp++) {
            float A[16][16], B[16][8], C[16][8];
            for (int L = 0; L < 32; L++) {
                const uint32_t *f = w + (warp * 32 + L) * 8; int g = L / 4, t = L % 4;
                for (int r = 0; r < 4; r++) { int row = g + 8 * (r & 1), k = 2 * t + 8 * (r >> 1); A[row][k] = h2f(f[r] & 0xffff); A[row][k + 1] = h2f(f[r] >> 16); }
                for (int r = 0; r < 2; r++) { int k = 2 * t + 8 * r; B[k][g] = h2f(f[4 + r] & 0xffff); B[k + 1][g] = h2f(f[4 + r] >> 16); }
                for (int r = 0; r < 2; r++) { int row = g + 8 * r; C[row][2 * t] = h2f(f[6 + r] & 0xffff); C[row][2 * t + 1] = h2f(f[6 + r] >> 16); }
            }
            for (int L = 0; L < 32; L++) {
                int g = L / 4, t = L % 4; const uint32_t *r = o + (warp * 32 + L) * 2;
                for (int q2 = 0; q2 < 2; q2++) for (int j = 0; j < 2; j++) {
                    int row = g + 8 * q2, col = 2 * t + j; float ref = C[row][col];
                    for (int k = 0; k < 16; k++) ref += A[row][k] * B[k][col];
                    double d = fabs(h2f((r[q2] >> (16 * j)) & 0xffff) - ref);
                    if (d > worst) worst = d;
                    bad += d > 0.01 + fabs(ref) * 0.004;
                }
            }
        }
        printf("%d SIMD-groups, max abs error %.4g, %ld outputs over tolerance\n", n / 32, worst, bad);
        return bad != 0;
    }
}
'''


class HmmaRewrite(unittest.TestCase):
    def test_body_replaced_with_its_arguments(self):
        out = hmma_msl(HEAD + HELPER + KERNEL)
        body = out[out.index('int dmsh_hmma16816_f16('):out.index('kernel void check')]
        self.assertEqual(body.count('simdgroup_multiply_accumulate('), 4)
        self.assertIn('threadgroup uint* s = &_34[uint(_42962)];', body)        # the scratch array at the block offset
        self.assertIn('uint g = uint(_42961) >> 2', body)                        # the lane
        self.assertIn('s[g * 8 + t] = uint(_42963)', body)                       # A0
        self.assertIn('s[192 + (g + 8) * 4 + t] = uint(_42970)', body)           # C1
        self.assertIn('_42971 = as_type<int>(d1);', body)                        # the out argument
        self.assertIn('return as_type<int>(d0);', body)
        self.assertNotIn('_42974', body)                                         # the translator's body is gone, the braces of its blocks too
        self.assertIn(KERNEL.strip(), out)

    def test_other_shaders_unchanged(self):
        src = HEAD + KERNEL.replace('dmsh_hmma16816_f16(l, blk, a0, a1, a2, a3, b0, b1, c0, c1, d1, scr)', 'a0')
        self.assertEqual(hmma_msl(src), src)

    def test_unexpected_signature_refused(self):
        with self.assertRaises(ValueError):
            hmma_msl(HEAD + HELPER.replace('thread const int& _42970, ', ''))


@unittest.skipUnless(shutil.which('xcrun') and shutil.which('clang'), 'needs Xcode')
class HmmaOnGpu(unittest.TestCase):
    def test_matches_reference(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / 'h.metal').write_text(hmma_msl(HEAD + HELPER + KERNEL))
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', '-fpreserve-invariance', str(d / 'h.metal'), '-o', str(d / 'h.air')], check=True)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / 'h.air'), '-o', str(d / 'h.metallib')], check=True)
            (d / 'run.m').write_text(RUNNER)
            subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
            r = subprocess.run([str(d / 'run'), str(d / 'h.metallib')], capture_output=True, text=True)
            if r.returncode == 2: self.skipTest(r.stdout.strip())
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
