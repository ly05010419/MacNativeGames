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

    @unittest.skipUnless(__import__('sys').platform == 'darwin', 'MetalFX requires macOS')
    def test_version_gate_and_interpolation_restore(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang unavailable')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as tmp:
            for definitions in ([], ['TOTK_GAME_V143'], ['GAME_BOTW'], ['GAME_BOTW', 'GAME_BOTW190']):
                exe = Path(tmp) / 'platform-probe'
                subprocess.run([compiler, '-std=c11', '-fobjc-arc', '-Wno-deprecated-declarations',
                                *['-D' + d + '=1' for d in definitions],
                                str(ROOT / 'tests/hdr_platform_probe.m'), str(gfx / 'totk_mfx.m'),
                                '-framework', 'Metal', '-framework', 'MetalFX', '-framework', 'QuartzCore',
                                '-framework', 'Foundation', '-o', str(exe)], check=True, capture_output=True)
                subprocess.run([str(exe)], env=dict(os.environ, TOTK_HDR='1', TOTK_HDR_CONTROL='1'), check=True, capture_output=True)
