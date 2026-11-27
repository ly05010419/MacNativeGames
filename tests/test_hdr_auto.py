"""The HDR / EDR choice off / on / auto (games/totk/runtime/hdr_auto.h, frontend.m front_hdr_auto): the settings probe's
hdr mode. No application, window, GPU or player data; the screen's potential EDR value comes from the probe."""
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
class HdrChoice(unittest.TestCase):
    def run_checked(self, command, **kwargs):
        result = subprocess.run(command, capture_output=True, text=True, timeout=90, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_hdr_choice(self):
        # Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 have the HDR rows; TOTK 1.2.1 (base) and BotW 1.6.0 have none
        versions = {'base': [], 'v143': ['-DTOTK_GAME_V143=1'], 'botw160': ['-DGAME_BOTW160=1', '-DGAME_BOTW=1'], 'botw190': ['-DGAME_BOTW190=1', '-DGAME_BOTW=1']}
        with tempfile.TemporaryDirectory() as directory:
            for version, flags in versions.items():
                with self.subTest(version=version):
                    executable = Path(directory) / version
                    self.run_checked(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                      '-Wno-deprecated-declarations', '-I' + str(GFX / 'include'),
                                      '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                                      str(ROOT / 'tests/settings_panel_probe.m'),
                                      str(ROOT / 'tests/settings_output_fixture.c'),
                                      str(GFX / 'totk_mfx.m'), str(GFX / 'totk_mfx_up.m'),
                                      str(GFX / 'totk_taa.m'), str(GFX / 'fps_overlay.c'),
                                      '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                                      '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable), *flags])
                    folder = Path(directory) / (version + '-settings')
                    folder.mkdir()
                    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_', 'MNG_'))}
                    env.update(SETTINGS_TEST_DIR=str(folder), TOTK_UI_LANG='en')
                    output = self.run_checked([str(executable), 'hdr'], env=env)
                    self.assertIn('pass: HDR / EDR choice', output)


if __name__ == '__main__':
    unittest.main()
