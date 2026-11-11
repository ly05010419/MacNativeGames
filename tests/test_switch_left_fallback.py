"""Captured Zeus 2 raw movement must survive a stuck native left-stick profile."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
MKW = Path(__file__).resolve().parents[1] / 'games/mkw'


@unittest.skipUnless(shutil.which('xcrun'), 'needs Apple command line tools')
class SwitchLeftFallbackTests(unittest.TestCase):
    def test_captured_motion_and_lifecycle(self):
        self.run_probe(TOTK)

    def test_mkw_keeps_raw_input_once_confirmed(self):
        self.run_probe(MKW)

    def run_probe(self, game):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            build = subprocess.run([
                'xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                f'-I{game}/runtime', str(game / 'tests/switch_left/probe.c'),
                '-o', str(probe),
            ], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(probe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn('PASS:', run.stdout)
