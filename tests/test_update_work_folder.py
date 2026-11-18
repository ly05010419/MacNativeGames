"""A runtime update run without --work uses the work folder the app records (MNGWorkFolder), not a new one next to the app.

2026-11-11: an update of the desktop Tears of the Kingdom app without --work made a second 2.2 GB TOTK_build in the folder
that holds the apps (the one for what must be kept), and the updated app then recorded it, although the app recorded the
work folder of its build. The in-app hint tells players to run `update_runtime.py "<app>"` without --work, so this is the
path players take too. Every build and update records the folder (build_app.record_update_source).
"""
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402


def app_with(folder, info):
    app = Path(folder) / 'Tears of the Kingdom.app'
    (app / 'Contents').mkdir(parents=True)
    (app / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
    return app


class RecordedWorkFolder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_recorded_folder_is_used(self):
        work = self.base / 'scratch/build/totk'; work.mkdir(parents=True)
        app = app_with(self.base / 'apps', {'MNGWorkFolder': str(work)})
        folder, why = build_app.recorded_work_folder(app, 'TOTK_build')
        self.assertEqual(folder, work.resolve())
        self.assertIn('recorded', why)

    def test_an_app_without_a_record_falls_back_next_to_it(self):
        app = app_with(self.base / 'apps', {})
        folder, why = build_app.recorded_work_folder(app, 'TOTK_build')
        self.assertEqual(folder, app.parent / 'TOTK_build')
        self.assertIn('records no work folder', why)

    def test_a_deleted_work_folder_is_made_again_where_it_was(self):
        # a player deleted the work folder to free space: the app carries its game data, so a new one there is enough
        work = self.base / 'work/totk'; work.parent.mkdir(parents=True)
        app = app_with(self.base / 'apps', {'MNGWorkFolder': str(work)})
        folder, why = build_app.recorded_work_folder(app, 'TOTK_build')
        self.assertEqual(folder, work)
        self.assertIn('made again', why)
        self.assertFalse((app.parent / 'TOTK_build').exists())

    def test_a_work_folder_on_a_missing_disk_is_an_error_not_a_new_folder_next_to_the_app(self):
        # Pokopia 11-11: the app recorded a folder on a volume deleted since; the fallback would have made one among the apps
        app = app_with(self.base / 'apps', {'MNGWorkFolder': str(self.base / 'old-volume/build/work')})
        with self.assertRaisesRegex(build_app.BuildError, 'connect that disk.*--work'):
            build_app.recorded_work_folder(app, 'TOTK_build')
        self.assertFalse((app.parent / 'TOTK_build').exists())

    def test_a_damaged_info_plist_falls_back(self):
        app = self.base / 'apps/Broken.app'; (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(b'not a plist')
        folder, _ = build_app.recorded_work_folder(app, 'BotW_build')
        self.assertEqual(folder, app.parent / 'BotW_build')


class UpdatesUseIt(unittest.TestCase):
    """Both update paths take their default from recorded_work_folder; --work still wins."""
    def test_tears_breath_of_the_wild_and_the_switch_2_games(self):
        # one default for every game update_runtime.py updates (old_work_name names the folder an app that records none
        # used, beside it; 32247916 added Bananza and Pokopia, which had only the full build until then)
        source = (ROOT / 'update_runtime.py').read_text()
        self.assertIn("build_app.recorded_work_folder(app, old_work_name(game))", source)
        self.assertNotIn("else app.parent / ('BotW_build' if game == 'botw' else 'TOTK_build')", source)
        import update_runtime
        self.assertEqual({g: update_runtime.old_work_name(g) for g in ('totk', 'botw', 'dkb', 'pkp')},
                         {'totk': 'TOTK_build', 'botw': 'BotW_build', 'dkb': 'Bananza_build', 'pkp': 'Pokopia_build'})

    def test_ocarina_of_time(self):
        source = (ROOT / 'update_runtime.py').read_text()
        self.assertIn("build_app.recorded_work_folder(app, 'OcarinaOfTime_build')", source)
        oot = (ROOT / 'games/oot/build.py').read_text()
        self.assertNotIn("else app.parent/'OcarinaOfTime_build'", oot)


if __name__ == '__main__':
    unittest.main()
