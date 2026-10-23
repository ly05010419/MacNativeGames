"""Synthetic scissor mapping/caller/ABI regressions; no game or GPU."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class BotwUIScissor(unittest.TestCase):
    def test_geometry_guards_and_version_abi(self):
        compiler = shutil.which('cc')
        if not compiler:
            self.skipTest('A C compiler is required')
        for defines in [[], ['-DGAME_BOTW=1'], ['-DGAME_BOTW=1', '-DGAME_BOTW190=1']]:
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as directory:
                executable = Path(directory) / 'scissor'
                build = subprocess.run(
                    [compiler, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                     *defines, '-I' + str(NVN / 'include'),
                     str(ROOT / 'tests/test_botw_ui_scissor.c'), '-lm', '-o', str(executable)],
                    capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('scissor geometry, caller guards and ABI passed', run.stdout)
