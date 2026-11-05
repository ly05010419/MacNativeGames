"""Exercise the actual Mac settings panel (key 1) without game data or GPU work."""
from pathlib import Path
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == 'darwin', 'AppKit requires macOS')
class SettingsPanel(unittest.TestCase):
    def test_vr_rows_use_supported_game_features(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as directory:
            for version in ('TOTK_GAME_V143', 'GAME_BOTW160', 'GAME_BOTW190'):
                executable = Path(directory) / version
                command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                           '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'), str(ROOT / 'tests/settings_output_fixture.c'),
                           *[str(gfx / f) for f in ('totk_mfx.m', 'totk_mfx_up.m', 'totk_taa.m', 'fps_overlay.c')],
                           '-framework', 'Cocoa', '-framework', 'Metal', '-framework', 'MetalFX',
                           '-framework', 'QuartzCore', '-framework', 'CoreVideo', '-o', str(executable),
                           '-D' + version + '=1']
                if version.startswith('GAME_BOTW'):
                    command.append('-DGAME_BOTW=1')
                result = subprocess.run(command, capture_output=True, text=True, timeout=90)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                result = subprocess.run([str(executable), 'vr-rows'], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('pass: supported VR settings rows', result.stdout)

    def test_update_page_never_forks(self):
        self.assertNotIn('fork(', (ROOT / 'games/totk/runtime/update_tab.h').read_text(encoding='utf-8'))   # the game's address space is huge

    def test_current_modes_and_refresh(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True)
        if int(sdk.split('.')[0]) < 26 or int(platform.mac_ver()[0].split('.')[0]) < 26:
            self.skipTest('The production Metal 4 mode implementations require macOS 26 and its SDK')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as directory:
            for version in ('', 'TOTK_GAME_V121', 'TOTK_GAME_V143', 'GAME_BOTW160', 'GAME_BOTW190'):
                executable = Path(directory) / ('settings-panel-' + (version or 'base'))
                command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra',
                           '-Werror', '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/settings_panel_probe.m'),
                           str(ROOT / 'tests/settings_output_fixture.c'),
                           str(gfx / 'totk_mfx.m'), str(gfx / 'totk_mfx_up.m'), str(gfx / 'totk_taa.m'),
                           str(gfx / 'fps_overlay.c'),
                           '-framework', 'Cocoa', '-framework', 'Metal',
                           '-framework', 'MetalFX', '-framework', 'QuartzCore',
                           '-framework', 'CoreVideo', '-o', str(executable)]
                if version:
                    command.append('-D' + version + '=1')
                if version.startswith('GAME_BOTW'):
                    command.append('-DGAME_BOTW=1')
                build = subprocess.run(command, capture_output=True, text=True, timeout=90)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for layout in ('compact', 'corners'):
                    for overridden in (False, True):
                        with self.subTest(version=version, layout=layout, overridden=overridden):
                            env = os.environ.copy()
                            env.update(TOTK_UI_LANG='en', TOTK_HUD=layout,
                                       TOTK_MFX='interp' if overridden else '0',
                                       TOTK_MFX_OUTPUT='1' if overridden else '0',
                                       TOTK_MFX_UPSCALE='0', TOTK_TAA='0')
                            run = subprocess.run([str(executable)], env=env,
                                                 capture_output=True, text=True, timeout=35)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pass: settings panel', run.stdout)
                settings = Path(directory) / ('saved-' + (version or 'base'))
                settings.mkdir()
                env = {k: v for k, v in os.environ.items() if k not in ('TOTK_MFX', 'TOTK_TAA', 'TOTK_ASPECT', 'TOTK_OUTPUT_RES')}
                env.update(TOTK_UI_LANG='en', SETTINGS_TEST_DIR=str(settings))
                if version == 'TOTK_GAME_V143':
                    aa_cases = [
                        ('defaults', {}, 1, 0, 2),
                        ('stock-off', {'graphics-fxaa.txt': 'off'}, 0, 0, 2),
                        ('split-fxaa', {'graphics-fxaa.txt': 'off', 'graphics-aa-method.txt': '1',
                                        'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '0'}, 1, 1, 0),
                        ('split-smaa', {'graphics-aa-method.txt': '2',
                                        'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '3'}, 2, 1, 3),
                        ('split-off', {'graphics-aa-method.txt': '0',
                                       'graphics-fxaa-quality.txt': '1', 'graphics-smaa-quality.txt': '3'}, 0, 1, 3),
                        ('corrupt', {'graphics-aa-method.txt': '9', 'graphics-fxaa-quality.txt': '8',
                                     'graphics-smaa-quality.txt': '-1'}, 1, 0, 2),
                        ('corrupt-qualities', {'graphics-aa-method.txt': '2',
                                              'graphics-fxaa-quality.txt': '99', 'graphics-smaa-quality.txt': 'nan'}, 2, 0, 2),
                    ]
                    for name, files, method, fxaa, smaa in aa_cases:
                        with self.subTest(aa_preferences=name):
                            prefs = Path(directory) / name
                            prefs.mkdir()
                            for filename, value in files.items():
                                (prefs / filename).write_text(value + '\n')
                            current = dict(env, SETTINGS_TEST_DIR=str(prefs), SETTINGS_TEST_AA_METHOD=str(method),
                                           SETTINGS_TEST_FXAA_QUALITY=str(fxaa), SETTINGS_TEST_SMAA_QUALITY=str(smaa))
                            run = subprocess.run([str(executable), 'aa'], env=current, capture_output=True, text=True, timeout=10)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pass: AA preferences', run.stdout)
                run = subprocess.run([str(executable), 'update'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('pass: update page', run.stdout)
                for action in ('save', 'restore', 'override'):
                    current = dict(env)
                    if action == 'override':
                        current.update(TOTK_MFX='interpmax', TOTK_TAA='0', TOTK_ASPECT='16:9', TOTK_OUTPUT_RES='1920x1080')
                    run = subprocess.run([str(executable), action], env=current, capture_output=True, text=True, timeout=35)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('pass: settings persistence', run.stdout)
                (settings / 'graphics-frame.txt').write_text('lock-60\n')
                run = subprocess.run([str(executable), 'lock'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                for name, value in {'graphics-overlay.txt': 'invalid', 'graphics-frame.txt': 'lock-60junk',
                                    'graphics-output.txt': '999', 'graphics-aspect.txt': 'nan', 'graphics-taa.txt': '3'}.items():
                    (settings / name).write_text(value)
                run = subprocess.run([str(executable), 'corrupt'], env=env, capture_output=True, text=True, timeout=35)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
