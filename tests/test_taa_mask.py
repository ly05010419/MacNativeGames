"""Edge smoothing's reactive mask kernels (taa_kernels.h), run on the GPU on synthetic depth (tests/taa_mask_probe.m).
The mask decides where MetalFX and the fast resolve throw their history away: on a still scene it must stay 0 (or the
anti-aliasing is lost), where a surface moved in it must reach 1 (or the old surface trails). The kernels are compiled
from the same header totk_taa.m compiles, so the probe tests what the game runs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class Kernels(unittest.TestCase):
    def test_the_game_compiles_the_shared_header(self):
        taa = (GFX / 'totk_taa.m').read_text()
        self.assertIn('#include "taa_kernels.h"', taa)
        self.assertIn('@(taa_kernels_msl)', taa)
        self.assertNotIn('kernel void taa_mv(', taa)   # one copy of the kernel: the header's


@unittest.skipUnless(sys.platform == 'darwin', 'Metal runs on macOS')
class MaskProbe(unittest.TestCase):
    def test_probe(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'taa-mask'
            build = subprocess.run(
                [compiler, '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I' + str(GFX), str(ROOT / 'tests/taa_mask_probe.m'),
                 '-framework', 'Foundation', '-framework', 'Metal', '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            if run.returncode == 77:
                self.skipTest('no Metal device')
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            for case in ('static_scene_zero', 'jump_20pct_full', 'jump_7pct_partial', 'prev_invalid_zero',
                         'thin_strand_1px_shift_not_masked_known_limit'):
                self.assertIn(case + ' ok', run.stdout)


if __name__ == '__main__':
    unittest.main()
