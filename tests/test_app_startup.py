"""Packaged startup stays in the main executable and finds every shader resource directory."""
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

import build_app


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'needs macOS and Xcode tools')
class AppStartup(unittest.TestCase):
    def test_all_versions_keep_bundle_resources_and_command_line_entry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            gfx = build_app.GAME / 'third_party/d2r_gfx/gfx/nvn'
            for version, expected in build_app.VERSIONS.items():
                with self.subTest(version=version):
                    game = 'botw' if version.startswith('botw') else 'totk'
                    name, bundle_id, executable = build_app.APPS[game]
                    app = root / f'{version} original.app'
                    binary = app / f'Contents/MacOS/{executable}'
                    binary.parent.mkdir(parents=True)
                    resources = app / 'Contents/Resources/totk'
                    (resources / 'build').mkdir(parents=True)
                    # An exec handoff would try to run these invalid bytes instead of calling back into this executable.
                    (resources / 'build/totk-native').write_text('not an executable')
                    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(dict(
                        CFBundleExecutable=executable, CFBundleIdentifier=bundle_id, CFBundleName=name,
                        CFBundlePackageType='APPL', CFBundleShortVersionString=expected['display_version'],
                        LSSupportsGameMode=True, LSApplicationCategoryType='public.app-category.action-games')))
                    defines = [] if version == 'v0' else [f'-DGAME_{version.upper()}=1'] if game == 'botw' else [f'-DTOTK_GAME_{version.upper()}=1']
                    subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-I'+str(gfx), '-I'+str(build_app.GAME / 'runtime'), *defines,
                        str(build_app.REPO / 'tests/app_startup_probe.m'), str(build_app.GAME / 'app/launcher.m'),
                        '-framework', 'AppKit', '-o', str(binary)], check=True, capture_output=True, text=True, timeout=60)
                    subprocess.run(['codesign', '--force', '--sign', '-', app], check=True, capture_output=True, timeout=30)
                    app = app.rename(root / f'{version} moved.app')
                    binary = app / f'Contents/MacOS/{executable}'
                    resources = app / 'Contents/Resources/totk'
                    run = root / f'{version}-data'
                    run.mkdir()
                    env = os.environ.copy()
                    env.update(TOTK_PROFILE_DIR=str(run / 'profile'), TOTK_SHADER_CACHE=str(run / 'cache'),
                        TOTK_GAME_SLOT_DIR=str(run / 'slot'), TOTK_INPUT_FILE=str(run / 'input.txt'),
                        TOTK_CONTROLS=str(run / 'controls.txt'), TOTK_PICTURE_MODE=str(run / 'picture.txt'),
                        TOTK_SHADER_CONTRIB=str(run / 'contributions.txt'), MNG_TEST_HOME=str(run / 'home'),
                        TOTK_SLOT_OWNER='codex', TOTK_DEBUG_LOG='1', MNG_STARTUP_REPORT=str(run / 'report.json'))
                    check = subprocess.run([binary, '--check'], check=True, capture_output=True, text=True, env=env, timeout=15)
                    self.assertIn('ok: game version '+expected['display_version'], check.stdout)
                    self.assertFalse((run / 'report.json').exists())
                    process = subprocess.Popen([binary], env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    stdout, stderr = process.communicate(timeout=15)
                    self.assertEqual(process.returncode, 0, stdout + stderr)
                    report = json.loads((run / 'report.json').read_text())
                    self.assertEqual(report['calls'], 1)
                    self.assertEqual(report['pid'], process.pid)
                    self.assertEqual(report['bundle'], bundle_id)
                    self.assertEqual(Path(report['executable']).resolve(), binary.resolve())
                    self.assertEqual((report['modules'], report['mode']), ('rewritten/'+version, 'boot'))
                    self.assertEqual(Path(report['cwd']).resolve(), resources.resolve())
                    self.assertEqual(Path(report['resources']).resolve(), (resources / 'build').resolve())
                    for key, leaf in (('profile', 'profile'), ('cache', 'cache'), ('input', 'input.txt')):
                        self.assertEqual(Path(report[key]), run / leaf)
                    subprocess.run(['codesign', '--verify', '--strict', app], check=True, capture_output=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
