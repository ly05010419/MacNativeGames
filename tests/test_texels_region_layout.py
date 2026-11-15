"""Run the actual regional MSL kernel as C++ against a byte-address oracle.

No Metal device or game data is used. Only Metal address spaces, kernel argument
attributes and scalar/vector types are adapted; the production address and copy
expressions are compiled verbatim. The oracle permutes GOB address bits rather
than repeating the production arithmetic. Switch 2's piece order is documented
in seedds/nxnative docs/reference/nvn-objects.md (fdb3710d); Tegra X1's order is
also documented beside nvn_deswizzle.c. This tests intra-level addressing, not
the independent texture layout builder's choice of mip/layer offsets.
"""
import argparse
import ast
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/third_party/d2r_gfx/gfx/tex/nvn_deswizzle_gpu.m'


def shader_source(source, ns2, directory):
    text = source.read_text()
    start = text.index('#ifdef GAME_NS2')
    end = text.index('\nint nvn_deswz_gpu_init')
    # Resolve the production string macros with the C preprocessor, including
    # the selected platform branch. No replacement formula lives in this test.
    unit = directory / 'shader.c'
    unit.write_text(text[start:end])
    command = ['clang', '-E', '-P', '-x', 'c']
    if ns2:
        command.append('-DGAME_NS2=1')
    preprocessed = subprocess.check_output(command + [str(unit)], text=True)
    shader = ''.join(ast.literal_eval(token) for token in
                     re.findall(r'"(?:[^"\\]|\\.)*"', preprocessed))
    return shader


def regional_kernel(source, ns2, directory):
    shader = shader_source(source, ns2, directory)
    kernel = shader[shader.index('struct R '):]
    kernel = re.sub(r'\[\[[^]]+\]\]', '', kernel)
    return kernel


HARNESS = r'''
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
using uint = uint32_t;
using ulong = uint64_t;
using uchar = unsigned char;
using std::min;
struct uint3 { uint x,y,z; };
struct alignas(16) uint4 { uint v[4]; };
#define constant const
#define device
#define kernel
@KERNEL@
static uint64_t checks;
static void check(bool good, const char *what) {
    ++checks;
    if (!good) { fprintf(stderr,"FAIL %s after %llu checks\n",what,(unsigned long long)checks); exit(1); }
}
// Bit positions within a 512-byte GOB, low to high. Bytes0..3 retain x0..3.
// NS2: y0,y1,x4,y2,x5; X1: y0,x4,y1,y2,x5.
static ulong oracle(const R& p,uint x,uint y,uint z) {
    const uint bh=1u<<p.th,bd=1u<<p.td;
    const ulong gx=(p.row_bytes+63u)/64u, by=(p.rows+8u*bh-1u)/(8u*bh);
    const ulong tile=((z/bd)*by+y/(8u*bh))*gx+x/64u;
    const ulong gob=(z%bd)*bh+(y/8u)%bh;
    uint bits[5];
    if (@NS2@) { bits[0]=y&1;bits[1]=(y>>1)&1;bits[2]=(x>>4)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1; }
    else { bits[0]=y&1;bits[1]=(x>>4)&1;bits[2]=(y>>1)&1;bits[3]=(y>>2)&1;bits[4]=(x>>5)&1; }
    ulong within=x&15;
    for(uint i=0;i<5;i++) within|=(ulong)bits[i]<<(i+4);
    return tile*(512ul*bh*bd)+gob*512ul+within;
}
static uchar value(uint x,uint y,uint z) {
    return (uchar)((x*73u)^(y*151u)^(z*199u)^(x>>3)^(y>>2));
}
static void run(R p) {
    const ulong tiles_x=(p.row_bytes+63u)/64u, tiles_y=(p.rows+(8u<<p.th)-1)/(8u<<p.th);
    const ulong size=p.pool_off+p.base+tiles_x*tiles_y*((p.slices+(1u<<p.td)-1)>>p.td)*(512ul<<p.th<<p.td)+32;
    const ulong linear_size=p.linear_off+p.img*p.d+32;
    std::vector<uchar> pool(size,0xcd),expected_pool(size,0xcd),linear(linear_size,0xa5),expected_linear(linear_size,0xa5);
    for(uint z=0;z<p.slices;z++)for(uint y=0;y<p.rows;y++)for(uint x=0;x<p.row_bytes;x++) {
        const ulong address=p.pool_off+p.base+oracle(p,x,y,z);
        check(address<size,"oracle bounds");
        pool[address]=value(x,y,z);
    }
    expected_pool=pool;
    for(uint z=0;z<p.d;z++)for(uint y=0;y<p.h;y++)for(uint x=0;x<p.w;x++)
        expected_linear[p.linear_off+z*p.img+y*p.pitch+x]=value(p.x+x,p.y+y,p.z+z);
    p.to_linear=1;
    for(uint z=0;z<p.d;z++)for(uint y=0;y<p.h;y++)for(uint x=0;x<(p.w+15)/16;x++)
        texels_region(pool.data(),linear.data(),p,{x,y,z});
    check(linear==expected_linear,"read pixels, tails and padding");
    check(pool==expected_pool,"read does not mutate pool");
    // Write new content into an oracle-populated pool; compare EVERY byte,
    // including untouched texels, tile padding, other layers and base offsets.
    for(uint z=0;z<p.d;z++)for(uint y=0;y<p.h;y++)for(uint x=0;x<p.w;x++) {
        const uchar v=(uchar)(value(p.x+x,p.y+y,p.z+z)^0x5b);
        linear[p.linear_off+z*p.img+y*p.pitch+x]=v;
        expected_pool[p.pool_off+p.base+oracle(p,p.x+x,p.y+y,p.z+z)]=v;
    }
    expected_linear=linear;p.to_linear=0;
    for(uint z=0;z<p.d;z++)for(uint y=0;y<p.h;y++)for(uint x=0;x<(p.w+15)/16;x++)
        texels_region(pool.data(),linear.data(),p,{x,y,z});
    check(pool==expected_pool,"write pixels and untouched pool bytes");
    check(linear==expected_linear,"write does not mutate linear");
}
int main() {
    // Exhaust all GOB bytes, tile heights/depths and block transitions.
    for(uint th=0;th<=4;th++)for(uint td=0;td<=2;td++) {
        R p={};p.row_bytes=130;p.rows=(8u<<th)+3;p.slices=(1u<<td)+1;p.th=th;p.td=td;
        for(uint z=0;z<p.slices;z++)for(uint y=0;y<p.rows;y++)for(uint x=0;x<p.row_bytes;x++)
            check(region_bl(p,x,y,z)==oracle(p,x,y,z),"GOB/tile address");
    }
    // NPOT ASTC8x8 1365x1365 becomes171x171 blocks of16bytes. Array/mip
    // base offsets are deliberately supplied, not derived by the tested code.
    // RGBA8 260x260 rows are1040bytes. Include small/tail mips and 3D tiles.
    const uint geometries[][5]={{2736,171,1,4,0},{4928,308,1,4,0},{1040,260,1,4,0},{1360,85,1,3,0},{16,1,1,0,0},{73,19,5,1,1}};
    for(const auto& g:geometries)for(uint unaligned=0;unaligned<2;unaligned++) {
        R p={};p.pool_off=32;p.linear_off=unaligned?7:16;p.base=65536;
        p.row_bytes=g[0];p.rows=g[1];p.slices=g[2];p.th=g[3];p.td=g[4];
        p.x=unaligned?3:0;p.y=p.rows>3?2:0;p.z=p.slices>1?1:0;
        p.w=p.row_bytes-p.x;p.h=p.rows-p.y;p.d=p.slices-p.z;
        p.pitch=(p.w+31)&~15u;p.img=p.pitch*p.h+32;
        run(p);
        // Small partial subregion crosses GOB boundaries while retaining neighbours.
        if(p.row_bytes>80&&p.rows>10) { p.x=61;p.y=5;p.w=19;p.h=6;p.pitch=37;p.img=p.pitch*p.h+5;run(p); }
    }
    printf("PASS platform=%s checks=%llu\n",@NS2@?"NS2":"X1",(unsigned long long)checks);
}
'''


def run_fixture(source, ns2):
    with tempfile.TemporaryDirectory(prefix='nvn-region-') as scratch:
        directory = Path(scratch)
        kernel = regional_kernel(source, ns2, directory)
        cpp = directory / 'region.cpp'
        cpp.write_text(HARNESS.replace('@KERNEL@', kernel).replace('@NS2@', str(int(ns2))))
        binary = directory / 'region'
        subprocess.run(['clang++', '-std=c++17', '-O1', '-Wall', '-Wextra',
                        str(cpp), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], text=True, capture_output=True)


@unittest.skipUnless(shutil.which('clang') and shutil.which('clang++'), 'clang is required')
class RegionalLayout(unittest.TestCase):
    def test_platforms(self):
        for ns2 in (False, True):
            with self.subTest(ns2=ns2):
                result = run_fixture(SOURCE, ns2)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())

    def test_original_x1_region_order_fails_ns2(self):
        text = SOURCE.read_text()
        offsets = re.findall(r'^#define GOB_OFFSET ("[^\n]+")$', text, re.MULTILINE)
        self.assertEqual(len(offsets), 2)
        with tempfile.TemporaryDirectory(prefix='nvn-region-negative-') as scratch:
            original_order = Path(scratch) / 'original-order.m'
            original_order.write_text(text.replace('#define GOB_OFFSET ' + offsets[0],
                                                   '#define GOB_OFFSET ' + offsets[1], 1))
            result = run_fixture(original_order, True)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('FAIL GOB/tile address after 17 checks', result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=SOURCE)
    parser.add_argument('--ns2-only', action='store_true')
    args = parser.parse_args()
    for platform in ((True,) if args.ns2_only else (False, True)):
        result = run_fixture(args.source, platform)
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise SystemExit(result.returncode)
