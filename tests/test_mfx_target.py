"""The fixed MetalFX upscale size: the fit, the "Upscale to" list and the saved choice (mfx_target.h)."""
from pathlib import Path
import os
import shutil
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


class MfxTarget(unittest.TestCase):
    def test_target_math(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'mfx-target'
            build = subprocess.run(
                [compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-I' + str(ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'),
                 str(ROOT / 'tests/mfx_target_probe.c'), '-o', str(executable)],
                capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: mfx target', run.stdout)

    @unittest.skipUnless(sys.platform == 'darwin', 'Mac source probe')
    def test_slider_stops(self):
        """The Display page's Resolution / MetalFX / Upscale to rows rest on labelled stops that apply on release,
        and a row can grey out: the panel offers both to every game, with the controller moving one stop at a time."""
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'stops'
            command = ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                       '-Wno-deprecated-declarations', '-I' + str(GFX / 'include'),
                       '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                       str(ROOT / 'tests/settings_panel_probe.m'),
                       str(ROOT / 'tests/settings_output_fixture.c'),
                       str(GFX / 'totk_mfx.m'), str(GFX / 'totk_mfx_up.m'),
                       str(GFX / 'totk_taa.m'), str(GFX / 'fps_overlay.c'),
                       '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                       '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable)]
            build = subprocess.run(command, capture_output=True, text=True, timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            folder = Path(directory) / 'settings'
            folder.mkdir()
            env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_', 'MNG_'))}
            env.update(SETTINGS_TEST_DIR=str(folder), TOTK_UI_LANG='en')
            run = subprocess.run([str(executable), 'stops'], capture_output=True, text=True, timeout=30, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('pass: slider stops', run.stdout)

    @unittest.skipUnless(sys.platform == 'darwin', 'Mac source probe')
    def test_target_rows(self):
        """Resolution, MetalFX and Upscale to on the Display page: today's pictures stay reachable and an old save comes
        back as the same picture, a fixed target reaches the graphics layer only while MetalFX is on, and a larger render
        drops a target that is no longer larger than it (settings_panel_probe.m mfx-target)."""
        versions = {'base': [], 'v143': ['-DTOTK_GAME_V143=1'],
                    'botw190': ['-DGAME_BOTW190=1', '-DGAME_BOTW=1'], 'botw160': ['-DGAME_BOTW160=1', '-DGAME_BOTW=1']}
        with tempfile.TemporaryDirectory() as directory:
            for version, flags in versions.items():
                with self.subTest(version=version):
                    executable = Path(directory) / version
                    command = ['xcrun', 'clang', '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                               '-Wno-deprecated-declarations', '-I' + str(GFX / 'include'),
                               '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                               str(ROOT / 'tests/settings_panel_probe.m'),
                               str(ROOT / 'games/totk/runtime/input_prompts.m'),
                               '-framework', 'CoreText', '-framework', 'CoreGraphics',
                               str(ROOT / 'tests/settings_output_fixture.c'),
                               str(GFX / 'totk_mfx.m'), str(GFX / 'totk_mfx_up.m'),
                               str(GFX / 'totk_taa.m'), str(GFX / 'fps_overlay.c'),
                               '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                               '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable), *flags]
                    build = subprocess.run(command, capture_output=True, text=True, timeout=120)
                    self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                    folder = Path(directory) / (version + '-settings')
                    folder.mkdir()
                    env = {k: v for k, v in os.environ.items() if not k.startswith(('TOTK_', 'DM_', 'MNG_'))}
                    env.update(SETTINGS_TEST_DIR=str(folder), TOTK_UI_LANG='en')
                    run = subprocess.run([str(executable), 'mfx-target'], capture_output=True, text=True, timeout=60, env=env)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr[-4000:])
                    self.assertIn('pass: mfx target rows', run.stdout)


if __name__ == '__main__':
    unittest.main()
