"""MetalFX's scaler cache keeps two configurations that take turns every present (frame interpolation on), even when it is
already full: before, each new build replaced the other one and a scaler was built at every present."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin', 'MetalFX runs on macOS')
class MetalFXScalerCache(unittest.TestCase):
    def test_two_configurations_in_turn_are_kept(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'mfx-up-cache'
            build = subprocess.run(
                [compiler, '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I' + str(GFX),
                 str(ROOT / 'tests/mfx_up_cache_probe.m'), str(GFX / 'totk_mfx_up.m'),
                 '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'MetalFX', '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: two configurations in turn are kept', run.stdout)


if __name__ == '__main__':
    unittest.main()
