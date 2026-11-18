"""The corner box's build line names the MNG Launcher that built the app before the commit.

User 2026-11-18, with a picture of the box ("版本 a0cd113"): "版本号 旁边 显示 launcher的版本号（launcher版本号放第一）".
A build or an update records the Launcher that runs it in the app's Info.plist (MNGLauncherVersion); the app's launcher
hands it to the runtime (TOTK_LAUNCHER_VERSION) and shared/runtime/hud_box.h shows "版本 1.0.6 · a0cd113". A Launcher
made from a checkout has no version of its own (its label says 开发版), so such a build says so too.
"""
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402


def launcher(folder, *, bundle_id=build_app.LAUNCHER_BUNDLE_ID, version='1.0.6', release=True):
    app = Path(folder) / 'MNG Launcher.app'
    (app / 'Contents/Resources/python/bin').mkdir(parents=True)
    info = {'CFBundleIdentifier': bundle_id, 'CFBundleShortVersionString': version}
    if release:
        info['MNGSourceArchiveSHA256'] = 'ab' * 32
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    return app


class WhichLauncher(unittest.TestCase):
    def version(self, *, named=None, python='/usr/bin/python3', checkout=False):
        with tempfile.TemporaryDirectory() as repo:
            if checkout:
                (Path(repo) / '.git').mkdir()
            env = {k: v for k, v in os.environ.items() if k != 'MNG_LAUNCHER_APP'}
            if named:
                env['MNG_LAUNCHER_APP'] = str(named)
            with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(build_app, 'REPO', Path(repo)), \
                    mock.patch.object(sys, 'executable', str(python)):
                return build_app.launcher_version()

    def test_the_release_that_runs_the_job(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(self.version(named=launcher(folder)), '1.0.6')

    def test_the_launcher_whose_python_runs_the_update(self):
        """An update started from a terminal with the Python the app recorded (MNGBuildPython)."""
        with tempfile.TemporaryDirectory() as folder:
            app = launcher(folder, version='1.0.7')
            self.assertEqual(self.version(python=app / 'Contents/Resources/python/bin/python3.14'), '1.0.7')

    def test_a_launcher_made_from_a_checkout_has_no_version(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(self.version(named=launcher(folder, version='1.0', release=False)), 'dev')

    def test_a_checkout_without_a_launcher_is_a_development_build(self):
        self.assertEqual(self.version(checkout=True), 'dev')

    def test_neither(self):
        self.assertEqual(self.version(), '')

    def test_another_app_is_not_the_launcher(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(self.version(named=launcher(folder, bundle_id='com.example.other')), '')

    def test_the_record_follows_this_build(self):
        with mock.patch.object(build_app, 'launcher_version', return_value='1.0.6'):
            info = {}
            build_app.record_launcher_version(info)
            self.assertEqual(info, {'MNGLauncherVersion': '1.0.6'})
        with mock.patch.object(build_app, 'launcher_version', return_value=''):
            build_app.record_launcher_version(info)   # an update without a Launcher drops the earlier build's
            self.assertEqual(info, {})


class FromTheAppToTheBox(unittest.TestCase):
    def test_every_way_an_app_is_made_records_it(self):
        self.assertIn('record_launcher_version(info)', (ROOT / 'build_app.py').read_text().split('def source_commit')[1])
        self.assertIn('build_app.record_launcher_version(info)', (ROOT / 'update_runtime.py').read_text())
        self.assertIn('build_app.record_launcher_version(info)', (ROOT / 'games/oot/build.py').read_text())

    def test_the_app_hands_it_to_the_runtime(self):
        for game in ('totk', 'oot'):
            with self.subTest(game=game):
                text = (ROOT / f'games/{game}/app/launcher.m').read_text()
                self.assertIn('objectForInfoDictionaryKey:@"MNGLauncherVersion"]', text)
                self.assertIn('setenv("TOTK_LAUNCHER_VERSION", builder.UTF8String, 1);', text)

    def test_the_box_puts_the_launcher_first(self):
        box = (ROOT / 'shared/runtime/hud_box.h').read_text()
        self.assertIn('hud_build_text(getenv("TOTK_LAUNCHER_VERSION"),getenv("TOTK_SOURCE_COMMIT"))', box)
        self.assertIn('@(TOTK_UI("版本 %s · %s","Launcher %s · build %s")),launcher,commit]', box)
        self.assertIn('@(TOTK_UI("版本 开发版 · %s","dev · build %s")),commit]', box)
        self.assertIn('@(TOTK_UI("版本 %s","build %s")),commit]', box)   # an app that recorded none: as before


if __name__ == '__main__':
    unittest.main()
