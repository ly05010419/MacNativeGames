"""Run the production Mac playtime counter probe without game data."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'),
                     'requires macOS and the Xcode command line tools')
class PlaytimeTests(unittest.TestCase):
    def test_session_and_persisted_total(self):
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory) / 'playtime-probe'
            build = subprocess.run(
                ['xcrun', 'clang', '-Wall', '-Wextra', '-Werror', '-fobjc-arc',
                 str(ROOT / 'games/totk/tests/playtime/probe.m'),
                 '-framework', 'Foundation', '-o', str(probe)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(probe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS:', run.stdout)
