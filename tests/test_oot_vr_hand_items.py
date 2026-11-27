"""Ocarina of Time's items in the player's hands, compiled and run natively: the hookshot, the boomerang, the bombs and the
bottles (zeldaVR/runtime/oot_vr_hookshot.h, oot_vr_boomerang.h, oot_vr_bomb.h, oot_vr_bottle.h, oot_vr_handitem.h)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class HandItems(unittest.TestCase):
    def run_c(self, name):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / name)
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / f'tests/test_oot_vr_{name}.c'), '-o', exe], check=True, capture_output=True, text=True)
            done = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            self.assertIn(': ok', done.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_hookshot(self):
        self.run_c('hookshot')

    def test_boomerang(self):
        self.run_c('boomerang')

    def test_bombs(self):
        self.run_c('bomb')

    def test_bottle(self):
        self.run_c('bottle')

    def test_the_bombs_float_thunks_run(self):
        """The two words before the branch in oot_vr_bombs.h's thunks put x4 in s0 and x3 in s1 (run natively on arm64)."""
        import platform
        if platform.system() != 'Darwin' or platform.machine() != 'arm64':
            self.skipTest('the words are arm64 code')
        host = (ROOT / 'games/oot/runtime/oot_vr_bombs.h').read_text()
        words = re.search(r'const uint32_t w\[3\] = \{ (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, oot_vr_b', host)
        self.assertIsNotNone(words)
        tmp = tempfile.mkdtemp()
        try:
            (Path(tmp) / 'thunk.s').write_text('.text\n.globl _thunk\n.p2align 2\n_thunk:\n .long %s\n .long %s\n b _target\n' % words.groups())
            (Path(tmp) / 'main.c').write_text(
                '#include <stdio.h>\n#include <string.h>\n#include <stdint.h>\n'
                'float thunk(void *, long, long, long, long);\n'
                'float target(void *p, long one, float s0, float s1) { return p && one == 1 ? s0 * 100 + s1 : -1; }\n'
                'static long bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }\n'
                'int main(void) { static int some; const float r = thunk(&some, 1, 0, bits(0.5f), bits(25.0f)); printf("%g\\n", (double)r); return r == 2500.5f ? 0 : 1; }\n')
            exe = str(Path(tmp) / 't')
            subprocess.run(['xcrun', 'clang', '-Wall', '-Werror', str(Path(tmp) / 'main.c'), str(Path(tmp) / 'thunk.s'), '-o', exe], check=True, capture_output=True, text=True)
            done = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_buttons_are_the_pads(self):
        """The item headers' button bits are the pad map's game buttons (zeldaVR/runtime/oot_vr_pad.h)."""
        pad = (ROOT / 'zeldaVR/runtime/oot_vr_pad.h').read_text()
        order = re.search(r'enum \{ OOT_SW_A = 0, ([^}]*)\}', pad).group(1).replace('\n', ' ')
        names = ['A'] + [n.strip().replace('OOT_SW_', '') for n in order.split(',') if n.strip()]
        items = (ROOT / 'zeldaVR/runtime/oot_vr_handitem.h').read_text()
        for name, bit in re.findall(r'OOT_IH_(A|B|ZL|ZR) = 1u << (\d+)', items):
            self.assertEqual(names.index(name), int(bit), name)


if __name__ == '__main__':
    unittest.main()
