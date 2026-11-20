"""Is a Mac migrated to MNG Launcher and the folder rule (tools/builder_jobs/migrate.py)? An AI opens the Launcher only
once it is (user 2026-11-12: "用 ai 打开 launcher 第一步要检查迁移情况，迁移完才能打开launcher")."""
from pathlib import Path
import plistlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO))
from tools.builder_jobs import migrate  # noqa: E402


class Migrate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        self.apps = self.tmp / 'disk/Applications'
        self.apps.mkdir(parents=True)

    def app(self, work=None, name='Tears of the Kingdom.app'):
        app = self.apps / name
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'MNGWorkFolder': str(work)} if work else {}))
        return app

    def state(self, app, game='totk', kind='express'):
        return migrate.app_state(app, game, kind)

    def test_a_work_folder_beside_the_apps_folder_is_migrated(self):
        work = self.tmp / 'disk/MacNativeGames-work/totk'
        work.mkdir(parents=True)
        self.assertEqual(self.state(self.app(work))['state'], 'ok')

    def test_any_folder_that_obeys_the_rule_is_migrated(self):
        work = self.tmp / 'disk/scratch/build/totk'   # this repository's own place on a developer's Mac
        work.mkdir(parents=True)
        self.assertEqual(self.state(self.app(work))['state'], 'ok')

    def test_a_work_folder_in_the_apps_folder_moves_to_the_rule_s_place(self):
        work = self.apps / 'TOTK_build'
        work.mkdir()
        s = self.state(self.app(work))
        self.assertEqual((s['state'], s['source'], s['target']), ('move', str(work), str(self.tmp / 'disk/MacNativeGames-work/totk')))

    def test_an_app_without_a_record_takes_its_old_folder_or_a_new_one(self):
        app = self.app()
        self.assertEqual(self.state(app)['state'], 'record')
        (self.apps / 'TOTK_build').mkdir()
        self.assertEqual(self.state(app)['state'], 'move')

    def test_a_game_whose_next_build_is_full_does_not_block(self):
        s = self.state(self.app(self.apps / 'Pokopia_build', name='Pokopia.app'), game='pkp', kind='full')
        self.assertEqual(s['state'], 'later')
        report = self.check([{'app': str(self.apps / 'Pokopia.app'), 'name': 'Pokopia', 'game': 'pkp', 'kind': 'full'}])
        self.assertTrue(report['done'])

    def test_a_disk_that_is_gone_blocks(self):
        self.assertEqual(self.state(self.app('/Volumes/no such disk/w/totk'))['state'], 'unreachable')

    def check(self, apps, launcher='ok'):
        with mock.patch.object(migrate, 'launcher_state', return_value={'state': launcher, 'reason': ''}):
            return migrate.check(apps)

    def test_done_needs_every_app_and_the_launcher(self):
        app = self.app(self.apps / 'TOTK_build')
        (self.apps / 'TOTK_build').mkdir()
        entry = [{'app': str(app), 'name': 'TotK', 'game': 'totk', 'kind': 'express'}]
        self.assertFalse(self.check(entry)['done'])
        self.assertFalse(self.check([], launcher='download')['done'])
        self.assertTrue(self.check([])['done'])

    def test_apply_renames_on_the_same_disk_then_records_with_an_update(self):
        old = self.apps / 'TOTK_build'
        old.mkdir()
        (old / 'marker').write_text('x')
        app = self.app(old)
        report = self.check([{'app': str(app), 'name': 'TotK', 'game': 'totk', 'kind': 'express'}])
        updates = []
        done = migrate.apply(report, lambda a, w: updates.append((a, w)) or 0)
        target = self.tmp / 'disk/MacNativeGames-work/totk'
        self.assertEqual((target / 'marker').read_text(), 'x')
        self.assertFalse(old.exists())
        self.assertEqual(updates, [(str(app), str(target))])
        self.assertEqual(len(done), 1)

    def test_apply_never_overwrites_a_folder_already_there(self):
        old = self.apps / 'TOTK_build'
        old.mkdir()
        (self.tmp / 'disk/MacNativeGames-work/totk').mkdir(parents=True)
        report = self.check([{'app': str(self.app(old)), 'name': 'TotK', 'game': 'totk', 'kind': 'express'}])
        updates = []
        migrate.apply(report, lambda a, w: updates.append(a) or 0)
        self.assertTrue(old.is_dir())
        self.assertEqual(updates, [])


class Launcher(unittest.TestCase):
    """The Launcher is installed, never made here (user 2026-11-20: its code is in a private repository of its own;
    "普通用户 只能下载launcher"): the released one, or a development one made against this checkout."""

    def state(self, info):
        with tempfile.TemporaryDirectory() as tmp:
            app = Path(tmp) / 'MNG Launcher.app'
            if info is not None:
                (app / 'Contents').mkdir(parents=True)
                (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            with mock.patch.object(migrate, 'LAUNCHER', app):
                return migrate.launcher_state()

    def test_the_released_launcher_is_enough(self):
        self.assertEqual(self.state({'MNGSourceArchiveSHA256': 'a' * 64})['state'], 'ok')

    def test_a_development_launcher_of_this_checkout_is_enough(self):
        self.assertEqual(self.state({'MNGRepo': str(REPO)})['state'], 'ok')

    def test_one_of_another_checkout_is_not(self):
        self.assertEqual(self.state({'MNGRepo': '/elsewhere/MacNativeGames'})['state'], 'other')

    def test_none_says_where_to_download_it(self):
        s = self.state(None)
        self.assertEqual(s['state'], 'download')
        self.assertIn(migrate.DOWNLOAD, s['reason'])

    def test_apply_makes_nothing(self):
        done = migrate.apply({'apps': [], 'launcher': {'state': 'download', 'reason': 'x'}}, lambda a, w: 0)
        self.assertEqual(done, [('MNG Launcher', 'x')])
        self.assertFalse(hasattr(migrate, 'make_launcher'))


if __name__ == '__main__':
    unittest.main()
