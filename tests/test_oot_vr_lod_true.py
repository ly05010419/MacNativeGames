"""Ocarina of Time in VR, detail by the distance to the eyes (vr.md row 18): the decoder, the x30 proof, the depth sums'
check, the veneers' words and the distance (zeldaVR/runtime/oot_vr_lod_true.h), the veneers run natively on the shared
hook body, and the site table (games/oot/runtime/oot_vr_lod_true.h)."""
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TrueDistanceDetail(unittest.TestCase):
    def test_the_parts(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'lod')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_oot_vr_lod_true.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    @unittest.skipUnless(platform.system() == 'Darwin' and platform.machine() == 'arm64', 'the words are arm64 code')
    def test_the_veneers_run(self):
        tmp = tempfile.mkdtemp()
        try:
            done = subprocess.run([sys.executable, '-B', str(ROOT / 'games/oot/tests/oot_vr_lod_true_native.py'), '--work-dir', tmp],
                                  capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn('returns to the site\'s next word', done.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_site_table(self):
        src = (ROOT / 'games/oot/runtime/oot_vr_lod_true.h').read_text()
        table = src[src.index('OOT_LT_SITES[] = {'):]
        table = table[:table.index('\n};')]
        sites = [(int(a, 16), int(d), int(s), int(o)) for a, d, s, o in re.findall(r'\{ (0x[0-9a-f]+), ([01]), (\d+), (\d+) \}', table)]
        self.assertEqual(len(sites), 193)                       # HookshotVR's 189 and the four it added
        offsets = [s[0] for s in sites]
        self.assertEqual(offsets, sorted(set(offsets)), 'the table is looked up by halves: sorted, no site twice')
        for off, d, s, o in sites:
            self.assertTrue(off % 4 == 0 and off < 0x3c461d0 and s != o and s <= 28 and o <= 28, hex(off))
        self.assertEqual(sum(d for _, d, _, _ in sites), 8)    # fadd s1, s0, s3 at eight of them

    def test_off_by_default(self):
        vr = (ROOT / 'zeldaVR/gfx/totk_vr.m').read_text()
        self.assertIn('int vr_oot_lod_true(void) { return vr_option("lod_true", 0) != 0; }', vr)
        page = (ROOT / 'games/oot/runtime/settings_game.h').read_text()
        self.assertIn('vr_oot_lod_true()', page)


if __name__ == '__main__':
    unittest.main()
