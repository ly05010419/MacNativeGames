"""TOTK_ASPECT parsing, key 4 and Vert+ (games/totk/runtime/aspect_math.h) and the UI squeeze
(games/totk/third_party/d2r_gfx/gfx/nvn/totk_ui_squeeze.h) without the game."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Aspect(unittest.TestCase):
    def test_aspect_math_and_stubs(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'aspect'
            build = subprocess.run(
                [compiler, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                 str(ROOT / 'tests/test_aspect.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('aspect ok', run.stdout)

