"""Optional arrow repeat callbacks, tested on fake objects without game files."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class AutoAttachTests(unittest.TestCase):
    def test_callbacks(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'auto-attach'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT / 'tests/test_auto_attach.c'), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS', run.stdout)
