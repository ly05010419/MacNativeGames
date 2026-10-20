"""Exercise the actual Mac shortcut panel without game data or GPU work."""
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
class ShortcutStatus(unittest.TestCase):
    def test_current_modes_and_refresh(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('clang is required')
        sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-version'], text=True)
        if int(sdk.split('.')[0]) < 26 or int(platform.mac_ver()[0].split('.')[0]) < 26:
            self.skipTest('The production Metal 4 mode implementations require macOS 26 and its SDK')
        gfx = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
        with tempfile.TemporaryDirectory() as directory:
            for version in ('', 'TOTK_GAME_V143'):
                executable = Path(directory) / ('shortcut-status-' + (version or 'base'))
                command = [compiler, '-std=c11', '-fobjc-arc', '-Wall', '-Wextra',
                           '-Werror', '-Wno-deprecated-declarations', '-I' + str(gfx / 'include'),
                           '-Wl,-undefined,dynamic_lookup', '-Wl,-dead_strip',
                           str(ROOT / 'tests/shortcut_status_probe.m'),
                           str(ROOT / 'tests/shortcut_output_fixture.c'),
                           str(gfx / 'totk_mfx.m'), str(gfx / 'totk_mfx_up.m'),
                           str(gfx / 'fps_overlay.c'),
                           str(ROOT / 'games/totk/runtime/settings.m'),
                           str(gfx / 'totk_sysload.c'),
                           '-framework', 'Cocoa', '-framework', 'Metal',
                           '-framework', 'MetalFX', '-framework', 'QuartzCore',
                           '-framework', 'CoreVideo', '-framework', 'IOKit',
                           '-framework', 'CoreFoundation', '-o', str(executable)]
                if version:
                    command.append('-D' + version + '=1')
                build = subprocess.run(command, capture_output=True, text=True, timeout=90)
                self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
                for layout in ('compact', 'corners'):
                    for overridden in (False, True):
                        with self.subTest(version=version, layout=layout, overridden=overridden):
                            env = os.environ.copy()
                            env.pop('TOTK_SLOT_OWNER', None)
                            for inherited in ('TOTK_MFX', 'TOTK_MFX_OUTPUT', 'TOTK_MFX_UPSCALE',
                                              'TOTK_OUTPUT_RES', 'TOTK_RENDER_RES', 'TOTK_ASPECT',
                                              'TOTK_FPS_OVERLAY', 'TOTK_SHADER_OVERLAY', 'TOTK_LOAD_OVERLAY'):
                                env.pop(inherited, None)
                            env.update(TOTK_UI_LANG='en', TOTK_HUD=layout,
                                       TOTK_SETTINGS_DIR=os.path.join(
                                           directory, 'settings-' + (version or 'base') + '-' + layout +
                                           ('-overridden' if overridden else '')))
                            if overridden:
                                # v2: a TOTK_* variable is an explicit lock, not just a starting value.
                                env.update(TOTK_MFX='interp', TOTK_MFX_OUTPUT='1', TOTK_MFX_UPSCALE='0')
                            run = subprocess.run([str(executable)], env=env,
                                                 capture_output=True, text=True, timeout=35)
                            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                            self.assertIn('pass: env-locked modes' if overridden
                                          else 'pass: current modes', run.stdout)
