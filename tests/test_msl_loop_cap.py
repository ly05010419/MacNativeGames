"""No loop of a translated shader may run unbounded (2026-11-22): Age of Calamity's first battle bound a texture buffer to a
slot its compute shaders read as a 2D texture; their light-list loop counts read garbage, the GPU never finished the command
buffer, WindowServer hung in the GPU driver and the Mac panicked (five times that day), and a killed process left the work
running. The cap is shared: shared/third_party/d2r_gfx/gfx/nvn/msl_loop_cap.h for a Mac host (every MSL it compiles) and
games/totk/ios/tools/msl_loop_cap.py, the same rewrite byte for byte, for every port's device seed (build_port_ipa.py caps
the sources before compile_seed.py compiles them). The maintainer, 2026-11-22: "用测试保证 不靠记忆"."""
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/msl_loop_cap.h'
TOOLS = ROOT / 'games/totk/ios/tools'
sys.path.insert(0, str(TOOLS))
import msl_loop_cap  # noqa: E402

# The ports whose Mac host caps the MSL it compiles (nvn_mtl.m totk_msl_library_ with the shared header), and those that
# still have to take it (each needs its own build check; their device seeds are capped by the shared build already).
CAPPED = {'aocx', 'p5r', 'w3'}
PENDING = {'acnh', 'kirby', 'mk8dx', 'odyssey', 'rdr'}

SAMPLE = '''kernel void main0(device uint* b [[buffer(0)]], uint3 id [[thread_position_in_grid]])
{
    int _68 = int(id.x);
    bool _76;
    do
    {
        int _82 = _68 + 256;
        _76 = uint(_82) >= b[0];
        _68 = _82;
    } while (!_76);
    for (int i = 0; i < int(b[1]); i++)
    {
        b[2] += 1u;
        while (b[3] != 0u)
        {
            b[3] -= 1u;
        }
    }
    for (;;)
    {
        break;
    }
}
'''


def loops(src):
    """(head line, the line after its brace) of every loop of SPIRV-Cross's layout."""
    lines = src.split('\n')
    return [(lines[i], lines[i + 2]) for i in range(len(lines) - 2)
            if msl_loop_cap.loop_head(lines[i].strip(' \t')) and lines[i + 1].strip() == '{']


class Rewrite(unittest.TestCase):
    def test_every_loop_carries_the_cap(self):
        capped = msl_loop_cap.cap(SAMPLE)
        found = loops(capped)
        self.assertEqual(len(found), 4, 'do, for, the nested while, for (;;)')
        for head, first in found:
            self.assertRegex(first.strip(), rf'^if \(\+\+_mng_loop_\d+ > {msl_loop_cap.LOOP_CAP}u\) break;$', head)
        counters = re.findall(r'uint (_mng_loop_\d+) = 0u;', capped)
        self.assertEqual(len(counters), 4)
        self.assertEqual(len(set(counters)), 4, 'one counter per loop')
        self.assertNotIn('_mng_loop_', SAMPLE)

    def test_nothing_else_changes(self):
        capped = msl_loop_cap.cap(SAMPLE)
        kept = [l for l in capped.split('\n') if '_mng_loop_' not in l]
        self.assertEqual(kept, SAMPLE.split('\n'))
        self.assertEqual(msl_loop_cap.cap(capped), capped, 'capping twice changes nothing')
        plain = 'fragment float4 f() { return float4(1); }\n'
        self.assertIs(msl_loop_cap.cap(plain), plain)
        self.assertIn('} while (!_76);', capped, 'a do-while\'s closing line is not a loop head')

    def test_the_cap_is_large_enough_for_real_loops(self):
        self.assertGreaterEqual(msl_loop_cap.LOOP_CAP, 1 << 16)
        self.assertIn(f'#define MSL_LOOP_CAP {msl_loop_cap.LOOP_CAP}u', HEADER.read_text())

    def test_folder(self):
        with tempfile.TemporaryDirectory() as d:
            a, b = Path(d, 'a_b.metal'), Path(d, 'b_b.metal')
            a.write_text(SAMPLE); b.write_text('fragment float4 f() { return float4(1); }\n')
            self.assertEqual(msl_loop_cap.cap_folder(d), 1)
            self.assertEqual(a.read_text(), msl_loop_cap.cap(SAMPLE))
            self.assertEqual(msl_loop_cap.cap_folder(d), 0)

    @unittest.skipUnless(sys.platform == 'darwin', 'Foundation')
    def test_the_hosts_header_gives_the_same_text(self):
        probe = r'''
#include "msl_loop_cap.h"
#include <stdio.h>
int main(int argc, char **argv) { @autoreleasepool {
    NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
    fputs(msl_loop_cap(src).UTF8String, stdout); return 0; } }
'''
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'p.m').write_text(probe)
            exe = Path(d) / 'p'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-I', str(HEADER.parent), str(Path(d) / 'p.m'),
                                    '-framework', 'Foundation', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            for text in (SAMPLE, msl_loop_cap.cap(SAMPLE), 'kernel void k() {}\n', SAMPLE.replace('    ', '\t')):
                (Path(d) / 's.metal').write_text(text)
                out = subprocess.run([str(exe), str(Path(d) / 's.metal')], capture_output=True, text=True).stdout
                self.assertEqual(out, msl_loop_cap.cap(text))


class Wiring(unittest.TestCase):
    def test_every_device_seed_is_capped(self):
        build = (TOOLS / 'build_port_ipa.py').read_text()
        self.assertIn('    if run:   # every port: no loop of a translated shader runs unbounded on the device', build)
        self.assertIn('        msl_loop_cap.cap_folder(out)', build)
        self.assertIn("if run: inputs.update(f'loopcap\\0{LOOP_CAP_ID}'.encode())", build, 'a new cap rebuilds the libraries')

    def test_the_capped_hosts_cap_what_they_compile(self):
        for game in sorted(CAPPED):
            with self.subTest(game=game):
                nvn = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn'
                link = nvn / 'msl_loop_cap.h'
                self.assertTrue(link.is_symlink() and link.resolve() == HEADER.resolve(), 'the shared header, linked')
                src = (nvn / 'nvn_mtl.m').read_text()
                body = src[src.index('static id<MTLLibrary> totk_msl_library_(NSString *msl, NSError **err) {'):]
                body = body[:body.index('\n}\n')]
                self.assertIn('NSString *src = msl_loop_cap(msl);', body)
                self.assertLess(body.index('msl_loop_cap(msl)'), body.index('newLibraryWithSource:src'))

    def test_every_port_has_decided(self):
        ports = {p.parts[-6] for p in ROOT.glob('games/*/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m')
                 if 'static id<MTLLibrary> totk_msl_library_(' in p.read_text()}
        self.assertEqual(ports - CAPPED - PENDING, set(), 'a new port: cap its host (CAPPED) or list it as PENDING')
        self.assertFalse(CAPPED & PENDING)


if __name__ == '__main__':
    unittest.main()
