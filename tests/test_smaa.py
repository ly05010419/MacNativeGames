"""SMAA's real Metal 4 passes on synthetic pixels; never opens or reads a game window."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin' and int(platform.mac_ver()[0].split('.')[0]) >= 26,
                     'requires macOS 26 Metal 4')
class SMAA(unittest.TestCase):
    def test_production_passes(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'smaa'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-O2', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/smaa_gpu_probe.m'), '-framework', 'Metal', '-framework', 'Foundation',
                            '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            if run.returncode == 77:
                self.skipTest(run.stdout.strip())
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('PASS: production SMAA presets', run.stdout)
