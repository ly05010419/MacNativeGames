"""Behavioral coverage for pause coordinator state machine and runtime/threads.c pause barrier."""
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/include'


class PauseStateTests(unittest.TestCase):
    def test_pause_state_transitions(self):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            self.skipTest('C compiler required')
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'pause-state'
            build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                                    str(ROOT / 'tests/test_pause_state.c'), '-o', str(binary)],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=5)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: pause state', run.stdout)

    @unittest.skipUnless(sys.platform == 'darwin' and platform.machine() == 'arm64', 'native Apple scheduler')
    def test_threads_pause_barrier_both_games(self):
        for defines in ([], ['-DGAME_BOTW=1']):
            with self.subTest(defines=defines), tempfile.TemporaryDirectory() as folder:
                binary = Path(folder) / 'threads-pause'
                build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                        '-I' + str(INCLUDE), *defines, '-Wl,-dead_strip',
                                        str(ROOT / 'tests/test_threads_pause.c'), '-o', str(binary)],
                                       capture_output=True, text=True, timeout=60)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for mode in ('0', '1'):
                    run = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=15)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass:', run.stdout)
