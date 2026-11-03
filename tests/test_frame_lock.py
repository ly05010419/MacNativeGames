"""Focused frame-lock checks: no application, window, GPU or player data."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/totk'
GFX = GAME / 'third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin', 'Mac source probe')
class FrameLock(unittest.TestCase):
    def run_checked(self, command, **kwargs):
        result = subprocess.run(command, capture_output=True, text=True, timeout=90, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_frame_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            for version in ('base', 'v143'):
                executable = Path(directory) / version
                command = ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                           '-Wno-deprecated-declarations', '-I' + str(GFX / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'),
                           str(ROOT / 'tests/settings_output_fixture.c'),
                           str(GFX / 'totk_mfx.m'), str(GFX / 'totk_mfx_up.m'),
                           str(GFX / 'totk_taa.m'), str(GFX / 'fps_overlay.c'),
                           '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                           '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable)]
                if version == 'v143':
                    command.append('-DTOTK_GAME_V143=1')
                self.run_checked(command)
                folder = Path(directory) / (version + '-settings')
                folder.mkdir()
                env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_', 'MNG_'))}
                env.update(SETTINGS_TEST_DIR=str(folder), TOTK_UI_LANG='en')
                output = self.run_checked([str(executable), 'frame'], env=env)
                self.assertIn('pass: frame lock choices', output)
                self.assertEqual(list(folder.iterdir()), [])

    def test_production_pacing(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'pacing'
            self.run_checked(['xcrun', 'clang', '-O1', '-Wno-unused-function', '-Wno-unused-parameter',
                              '-I' + str(GFX / 'include'), '-Wl,-dead_strip', '-Wl,-undefined,dynamic_lookup',
                              str(GAME / 'tests/fps_limit/pacing.c'), str(GAME / 'tests/fps_limit/hold.c'),
                              str(GFX / 'nvn_entry.S'), str(GFX / 'nvn_thunks.S'), '-o', str(executable)])
            output = self.run_checked([str(executable)])
            self.assertIn('PASS: 168 production pacing scenarios', output)


if __name__ == '__main__':
    unittest.main()
