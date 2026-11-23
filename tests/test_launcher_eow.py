"""The Legend of Zelda: Echoes of Wisdom in MNG Launcher (user 2026-11-17: "继续做这个 一样放在 launcher里"): its files are
recognised, the build runs games/eow/build.py, the installed app is found by its bundle ID and updated by the same script,
its card has the Controls (2026-11-17), Storage and Saves pages, and its app launcher (games/eow/app/launcher.c) refuses an app without its
runtime or game data."""
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.builder import games  # noqa: E402
from tools.builder_jobs import build_job, classify, installed, repo  # noqa: E402


def load_build():
    import importlib.util
    spec = importlib.util.spec_from_file_location('eow_build_test', ROOT / 'games/eow/build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Wiring(unittest.TestCase):
    def test_titles_versions_and_script(self):
        for title in ('01008CF01BAAC000', '01008CF01BAAC800'):
            self.assertEqual(repo.game_of_title(title), 'eow')
        expected = json.loads((ROOT / 'games/eow/assets/expected-v1.1.0.json').read_text())
        self.assertEqual(classify._supported('eow'), {0, expected['version']})
        with tempfile.TemporaryDirectory() as tmp:
            plan = {'game': 'eow', 'base': '/b.xci', 'keys': '/prod.keys', 'update': '/u.nsp', 'update_version': 196608,
                    'firmware': '/fw', 'blocks': []}
            script, argv, work, out, kind, game = build_job.command({'plan': plan, 'output_dir': tmp, 'work_root': tmp})
        self.assertEqual(script, ROOT / 'games/eow/build.py')
        self.assertEqual((kind, game, out.name), ('full', 'eow', 'The Legend of Zelda Echoes of Wisdom.app'))
        for flag, value in (('-k', '/prod.keys'), ('--update', '/u.nsp'), ('--firmware', '/fw')):
            self.assertEqual(argv[argv.index(flag) + 1], value)

    def test_app_identity_and_pages(self):
        build = load_build()
        self.assertEqual(build.APP_NAME, repo.GAMES['eow'])
        self.assertEqual(build.BUNDLE_ID, games.EOW.bundle_id)
        self.assertIn(games.EOW.bundle_id, installed.bundle_ids())
        self.assertIn(f"BUNDLE_ID = '{games.EOW.bundle_id}'", (ROOT / 'games/eow/scripts/package_app.py').read_text())
        # the data folder under MacNativeGames (shared/app/app_launcher.h launcher_data_folder's last argument)
        self.assertIn(f'"{games.EOW.data_folder}")', (ROOT / 'games/eow/app/launcher.c').read_text())
        self.assertEqual(games.PAGES['eow'], ('controls', 'storage', 'saves'))
        # the window's pages and its case (Model.swift, GameCatalog): checked in MNG-Launcher (tests/test_porting_contract.py)


class UpdateBlocker(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.app, self.work = root / 'E.app', root / 'work'
        (self.app / 'Contents').mkdir(parents=True)
        (self.app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.macnativegames.eow'}))

    def test_complete_work_folder_allows_the_update_and_each_part_is_required(self):
        w = self.work
        files = [w / 'venv/bin/python', w / 'extracted/v110/romfs/romfs.bin', w / 'build/shader-seed/seed-manifest.json',
                 *(w / f'rewritten/v110/{m}.patched.bin' for m in self.build.MODULES)]
        fonts = w / 'extracted/firmware/shared-fonts'
        rows = []
        for kind, name in enumerate(('standard', 'chinese-simplified', 'chinese-simplified-extension',
                                     'chinese-traditional', 'korean', 'nintendo-extension')):
            files.append(fonts / f'{kind}-{name}.bfttf')
            rows.append({'type': kind, 'name': name})
        for path in files:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('')
        (fonts / 'manifest.json').write_text(json.dumps({'status': 'pass', 'fonts': rows}))
        self.assertEqual(self.build.blocker(self.app, w), '')
        for path in files[:4]:
            path.unlink()
            self.assertIn('full build', self.build.blocker(self.app, w))
            path.write_text('')


@unittest.skipUnless(shutil.which('xcrun'), 'needs the Xcode command line tools')
class Launcher(unittest.TestCase):
    def test_check_refuses_an_app_without_runtime_or_game_data(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / 'E.app/Contents'
            (app / 'MacOS').mkdir(parents=True)
            exe = app / 'MacOS/Echoes of Wisdom'
            subprocess.run(['xcrun', 'clang', '-O2', '-Wall', '-Wextra', '-Werror', str(ROOT / 'games/eow/app/launcher.c'),
                            '-framework', 'CoreFoundation', '-o', str(exe)], check=True)
            self.assertNotEqual(subprocess.run([str(exe), '--check'], capture_output=True).returncode, 0)
            runtime = app / 'Resources/game/build/eow-native'
            runtime.parent.mkdir(parents=True)
            runtime.write_text('#!/bin/sh\n')
            os.chmod(runtime, 0o755)
            self.assertNotEqual(subprocess.run([str(exe), '--check'], capture_output=True).returncode, 0)
            index = app / 'Resources/game/romfs/runtime-index.bin'
            index.parent.mkdir(parents=True, exist_ok=True)
            index.write_bytes(b'')
            done = subprocess.run([str(exe), '--check'], capture_output=True, text=True)
            self.assertEqual(done.returncode, 0, done.stderr)
            self.assertIn('not launched', done.stdout)


if __name__ == '__main__':
    unittest.main()
