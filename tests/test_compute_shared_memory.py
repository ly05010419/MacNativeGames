"""Every Zelda-line port tells the shader translator a compute kernel's shared memory (shared compute_shared.h).

Root cause of Age of Calamity's first-battle GPU hang (2026-11-22, five panics of the maintainer's MacBook Air): the
particle kernel dc81d690 keeps 68 bytes per thread from byte 304 for 64 threads, 4656 bytes of shared memory (NVN control
section +0x754), but no port passed that size, so the translator declared its 4 KiB default; threads past it wrote outside
the threadgroup array, the counts its loops run to became garbage and the GPU never finished. Unfixed runs hung every
time (4 of 4), fixed runs never (3 of 3).

Guarded here:
- the shared header's rule (compiled and run): the size from +0x754 rounded up to 16, requested only above 4 KiB, and a
  cached translation whose threadgroup array is smaller is translated again;
- every port's runtime translation (totk_shader_rt.m) includes it and uses it in the request, the variant name and the
  cache check (or, for the four ports that already did, reads +0x754 itself);
- every offline seed tool that gives the translator a compute group size also gives it the shared memory."""
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/compute_shared.h'
ALREADY = {'ori', 'ot0', 'xc3', 'tomodachi'}   # passed control+0x754 before the shared header existed
NXNATIVE = {'aoc', 'la', 'ss', 'worldend', 'jamboree', 'mk8d'}   # nxnative's own runtime (shader_split.py answers it)


def port_runtime_files():
    """{port: its totk_shader_rt.m}, each real file once (oot shares Tears of the Kingdom's folder)."""
    seen, out = set(), {}
    for f in sorted(ROOT.glob('games/*/third_party/d2r_gfx/gfx/nvn/totk_shader_rt.m')):
        real = f.resolve()
        if real in seen:
            continue
        seen.add(real)
        out[f.parts[-6]] = f
    return out


class SharedHeader(unittest.TestCase):
    def test_the_rule_compiled_and_run(self):
        probe = r'''
#include <stdio.h>
#include "compute_shared.h"
static uint32_t at(uint32_t v) { static uint8_t c[0x800]; memcpy(c + 0x754, &v, 4); return totk_rt_compute_shared(c); }
static uint32_t rq(uint32_t v) { static uint8_t c[0x800]; memcpy(c + 0x754, &v, 4); return totk_rt_compute_shared_request(c); }
int main(void) {
    printf("%u %u %u %u %u\n", at(4656), at(4650), at(0), at(40000), at(32768));
    printf("%u %u %u\n", rq(4656), rq(4096), rq(2048));
    printf("%d %d %d %d %d\n",
        totk_rt_msl_shared_ok("threadgroup spvUnsafeArray<uint, 1024> _40;", 4656),
        totk_rt_msl_shared_ok("threadgroup spvUnsafeArray<uint, 1164> _40;", 4656),
        totk_rt_msl_shared_ok("kernel void main0() {}", 4656),
        totk_rt_msl_shared_ok("threadgroup spvUnsafeArray<uint, 1024> _40;", 4096),
        totk_rt_msl_shared_ok("threadgroup spvUnsafeArray<uint, 16> _a; threadgroup spvUnsafeArray<uint, 2048> _b;", 8192));
    return 0;
}'''
        with tempfile.TemporaryDirectory() as d:
            src, exe = Path(d) / 'p.c', Path(d) / 'p'
            src.write_text(probe)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Werror', '-I', str(HEADER.parent), str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.split('\n')
        self.assertEqual(out[0], '4656 4656 0 0 32768', 'control+0x754 rounded up to 16; none above 32 KiB')
        self.assertEqual(out[1], '4656 0 0', 'requested only above the 4 KiB default: other translations stay as they were')
        self.assertEqual(out[2], '0 1 1 1 1', 'a 4 KiB array for a 4656-byte kernel is translated again (dc81d690)')


class EveryPortsRuntime(unittest.TestCase):
    def test_every_port_uses_the_shared_header(self):
        ports = port_runtime_files()
        self.assertGreaterEqual(len(ports), 25)
        for port, f in ports.items():
            src = f.read_text()
            if port in ALREADY:
                self.assertIn('0x754', src, port)
                continue
            with self.subTest(port=port):
                self.assertIn('#include "compute_shared.h"', src)
                link = f.parent / 'compute_shared.h'
                self.assertTrue(link.is_symlink(), 'linked, not copied')
                self.assertEqual(link.resolve(), HEADER.resolve())
                self.assertRegex(src, r'rq\[@"shared"\] = @\((totk_rt_compute_shared_request\(control\)|big_shared)\)', 'the request carries it')
                self.assertRegex(src, r'(key|skey|k)\[@"shared"\] = @\(', 'its own variant name')
                self.assertIn('totk_rt_msl_shared_ok(msl.UTF8String, ', src, 'a too-small cached translation is not reused')

    def test_lookups_pass_the_kernels_need(self):
        for port, f in port_runtime_files().items():
            src = f.read_text()
            if 'static NSString *variant_lookup(' not in src:
                continue
            with self.subTest(port=port):
                self.assertIn('static NSString *variant_lookup(NSString *shader, NSDictionary *answers, uint32_t need_shared)', src)
                self.assertNotRegex(src, r'variant_lookup\(shader, answers\)', 'every caller passes the need')
                if 'g_seed_sealed && [g_seed_names containsObject:msl_name]' in src:
                    self.assertIn('(!need_shared && g_seed_sealed && [g_seed_names containsObject:msl_name]', src, 'a sealed seed is read for such a kernel')


class EverySeedTool(unittest.TestCase):
    def test_a_group_size_comes_with_the_shared_memory(self):
        local = re.compile(r"""\[['"]local['"]\] = list\(struct\.unpack_from\(['"]<III['"], control, 0x748\)\)""")
        found = 0
        for f in sorted(ROOT.glob('games/*/**/*.py')):
            if '__pycache__' in f.parts or f.parts[len(ROOT.parts) + 1] in NXNATIVE or 'probe' in f.name:
                continue
            src = f.read_text(errors='replace')
            for m in local.finditer(src):
                found += 1
                after = src[m.end():m.end() + 400]
                with self.subTest(file=str(f.relative_to(ROOT))):
                    self.assertRegex(after, r"0x754|big_shared\(control\)", 'the translator is told the shared memory')
                    self.assertRegex(after, r"4096|big_shared\(control\)", 'only above the 4 KiB default, as the runtime')
        self.assertGreaterEqual(found, 15)

    def test_tears_of_the_kingdoms_seed_rule_matches_the_header(self):
        import importlib.util, struct
        for f in ('games/totk/scripts/prepare_shader_seed.py', 'games/oot/scripts/prepare_shader_seed.py'):
            src = (ROOT / f).read_text()
            fn = src[src.index('def big_shared(control):'):src.index('def translation_input(')]
            ns = {'struct': struct}; exec(fn, ns)
            for raw, want in ((4656, 4656), (4650, 4656), (4096, 0), (0, 0), (40000, 0), (32768, 32768)):
                c = bytearray(0x800); struct.pack_into('<I', c, 0x754, raw)
                self.assertEqual(ns['big_shared'](bytes(c)), want, (f, raw))
            self.assertIn("(struct.pack('<I', big_shared(control)) if stage == 'cs' and big_shared(control) else b'')", src, 'the seed digest tells such kernels apart')


if __name__ == '__main__':
    unittest.main()
