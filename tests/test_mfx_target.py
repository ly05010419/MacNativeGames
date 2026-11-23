"""The fixed MetalFX upscale size: the fit, the "Upscale to" list and the saved choice (mfx_target.h)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MfxTarget(unittest.TestCase):
    def test_target_math(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'mfx-target'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'),
                 str(ROOT / 'tests/mfx_target_probe.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: mfx target', run.stdout)


if __name__ == '__main__':
    unittest.main()
