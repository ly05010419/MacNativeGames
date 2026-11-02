"""Exercise Odyssey's actual AppKit panel, input handling and saved settings without game data or GPU submission."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

GAME = Path(__file__).resolve().parents[1]
GFX = GAME / 'third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires macOS and Xcode')
class OdysseySettingsPanel(unittest.TestCase):
    def test_controller_navigation_and_shortcut(self):
        with tempfile.TemporaryDirectory(prefix='odyssey-controller-panel-') as directory:
            exe = Path(directory) / 'probe'
            build = subprocess.run(['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-deprecated-declarations', '-I' + str(GFX / 'include'), '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                                    str(GAME / 'tests/settings_controller/probe.m'), '-framework', 'Cocoa',
                                    '-framework', 'Metal', '-framework', 'QuartzCore', '-framework', 'CoreVideo',
                                    '-framework', 'GameController', '-framework', 'IOKit', '-o', str(exe)],
                                   capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe), directory], env=dict(os.environ, TOTK_UI_LANG='en'),
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: controller settings', run.stdout)

    def test_panel_and_persistence(self):
        with tempfile.TemporaryDirectory(prefix='odyssey-settings-') as directory:
            root = Path(directory)
            exe = root / 'panel'
            command = ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                       '-Wno-deprecated-declarations', '-Wno-unused-function', '-I' + str(GFX / 'include'),
                       '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                       str(GAME / 'tests/settings_panel/probe.m'), str(GAME / 'tests/settings_panel/output_fixture.c'),
                       str(GFX / 'totk_mfx.m'), str(GFX / 'totk_mfx_up.m'), str(GFX / 'totk_taa.m'), str(GFX / 'fps_overlay.c'),
                       '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX', '-framework', 'QuartzCore',
                       '-framework', 'CoreVideo', '-framework', 'IOKit', '-o', str(exe)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'ODYSSEY_'))}
            env.update(TOTK_UI_LANG='en', TOTK_TAA='0', TOTK_MFX_OUTPUT='0', TOTK_MFX_UPSCALE='0')
            for layout in ('compact', 'corners'):
                with self.subTest(layout=layout):
                    run = subprocess.run([str(exe)], env=dict(env, TOTK_HUD=layout), capture_output=True, text=True, timeout=40)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: settings panel', run.stdout)
            env.pop('TOTK_TAA')
            env.update(SETTINGS_TEST_DIR=str(root), ODYSSEY_GFX_SETTINGS=str(root / 'graphics.txt'))
            for action in ('save', 'restore', 'override'):
                current = dict(env)
                if action == 'override':
                    current.update(TOTK_MFX='0', TOTK_TAA='0', TOTK_ASPECT='16:9')
                run = subprocess.run([str(exe), action], env=current, capture_output=True, text=True, timeout=40)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: settings persistence', run.stdout)
            self.assertEqual((root / 'fps-graph.txt').read_text(), 'on\n')
            self.assertIn('frame_generation=on\n', (root / 'graphics.txt').read_text())


if __name__ == '__main__':
    unittest.main()
