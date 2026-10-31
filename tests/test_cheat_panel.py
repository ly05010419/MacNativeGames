"""Native panel behavior with synthetic data and isolated temporary settings."""
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'AppKit requires macOS')
class CheatPanelTests(unittest.TestCase):
    def test_filter_conflicts_persistence_and_capture(self):
        compiler = shutil.which('clang')
        if not compiler or int(platform.mac_ver()[0].split('.')[0]) < 26:
            self.skipTest('macOS 26 and clang required by existing platform fixture')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as d:
            binary = Path(d) / 'panel'
            command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                       '-Wno-deprecated-declarations', '-DTOTK_GAME_V121=1', '-I' + str(gfx / 'include'),
                       '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                       str(ROOT / 'tests/cheat_vm/panel_probe.m'),
                       str(ROOT / 'tests/settings_output_fixture.c')]
            command += [str(gfx / f) for f in ('totk_mfx.m', 'totk_mfx_up.m', 'totk_taa.m', 'fps_overlay.c')]
            for framework in ('Cocoa', 'Metal', 'MetalFX', 'QuartzCore', 'CoreVideo'):
                command += ['-framework', framework]
            subprocess.run(command + ['-o', str(binary)], check=True, capture_output=True, text=True, timeout=90)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=20)
            self.assertIn('pass: native cheat panel', result.stdout)
