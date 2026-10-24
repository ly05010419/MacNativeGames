"""Portable HDR headroom invariants; the Metal probe is run separately on a Mac."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HDRMathTests(unittest.TestCase):
    def test_headroom_and_reference_white(self):
        cc = shutil.which(os.environ.get('CC', 'cc'))
        if not cc:
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'hdr-math'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_hdr_math.c'), '-lm', '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
