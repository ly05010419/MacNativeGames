"""tools/builder/saves.py (S3): the save folders the builder finds, a backup that keeps the folder shape and a
backup.json, and a restore that writes over the profile without ever deleting a backup or a newer file."""
import json
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

from tools.builder import saves
from tools.builder.games import GAMES


def write(path, data=b'save'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


NOW = datetime(2026, 8, 17, 10, 30, 0).timestamp()


class Locations(unittest.TestCase):
    def test_the_profile_is_the_launchers_application_support_folder(self):
        home = Path('/Users/me')
        self.assertEqual(saves.profile_dir(GAMES['totk'], home, {}),
                         home / 'Library/Application Support/MacNativeGames/TOTK/profile')
        self.assertEqual(saves.profile_dir(GAMES['botw'], home, {}),
                         home / 'Library/Application Support/MacNativeGames/BotW/profile')

    def test_the_profile_environment_variable_moves_it(self):
        self.assertEqual(saves.profile_dir(GAMES['totk'], '/Users/me', {'TOTK_PROFILE_DIR': '/p/totk'}),
                         Path('/p/totk'))
        self.assertEqual(saves.profile_dir(GAMES['botw'], '/Users/me', {'TOTK_PROFILE_DIR': '/p/botw'}),
                         Path('/p/botw'))   # one launcher for both games: the same name

    def test_the_default_backup_root_is_in_documents(self):
        self.assertEqual(saves.default_backup_root('/Users/me'),
                         Path('/Users/me/Documents/MacNativeGames-Backups'))

    def test_a_missing_profile_is_reported_not_guessed(self):
        with tempfile.TemporaryDirectory() as folder:
            item = saves.source(GAMES['totk'], Path(folder), {})
        self.assertFalse(item.exists)
        self.assertEqual(item.files, ())

    def test_nxnative_folders_are_the_16_digit_title_ids_written(self):
        with tempfile.TemporaryDirectory() as folder:
            root = saves.nxnative_root(Path(folder))
            (root / '0100F2C0115B6000' / 'slot1').mkdir(parents=True)
            (root / '01007EF00011E000').mkdir()
            (root / 'not-a-title').mkdir()
            write(root / 'readme.txt', b'x')
            found = saves.nxnative_dirs(Path(folder))
            items = saves.nxnative_sources(Path(folder))
        self.assertEqual([title for title, _ in found], ['01007EF00011E000', '0100F2C0115B6000'])
        self.assertEqual([item.game for item in items], ['botw', 'totk'])
        self.assertTrue(all(item.kind == 'nxnative' for item in items))


class Files(unittest.TestCase):
    def test_a_folder_lists_relative_posix_paths_and_sizes(self):
        with tempfile.TemporaryDirectory() as folder:
            write(Path(folder) / 'slot/progress.sav', b'abc')
            write(Path(folder) / 'top.bin', b'1')
            found = saves.list_files(Path(folder))
        self.assertEqual([(f.path, f.size) for f in found], [('top.bin', 1), ('slot/progress.sav', 3)])

    def test_a_missing_folder_lists_nothing(self):
        self.assertEqual(saves.list_files('/no/such/folder'), ())


class Backup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.profile = self.root / 'profile'
        write(self.profile / 'slot1/progress.sav', b'alpha')
        write(self.profile / 'settings.bin', b'cfg')
        self.backups = self.root / 'backups'

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_backup_copies_the_files_and_writes_a_manifest(self):
        backup, error = saves.backup(self.profile, self.backups, 'totk', 'totk', NOW)
        self.assertIsNone(error)
        self.assertEqual(backup.directory.parent, self.backups)
        self.assertTrue(backup.directory.name.startswith('totk-'))
        self.assertTrue((backup.directory / 'slot1/progress.sav').is_file())
        manifest = json.loads((backup.directory / 'backup.json').read_text())
        self.assertEqual(manifest['format'], saves.FORMAT)
        self.assertEqual(manifest['game'], 'totk')
        self.assertEqual(manifest['source'], str(self.profile))
        self.assertEqual(manifest['created'], datetime.fromtimestamp(NOW).isoformat(timespec='seconds'))
        self.assertEqual([item['path'] for item in manifest['files']],
                         ['settings.bin', 'slot1/progress.sav'])
        self.assertTrue(all(len(item['sha256']) == 64 for item in manifest['files']))
        self.assertEqual(backup.bytes, 8)
        self.assertTrue((self.profile / 'slot1/progress.sav').is_file())   # the source is untouched

    def test_a_backup_reads_back_as_the_same_manifest(self):
        backup, _ = saves.backup(self.profile, self.backups, 'totk', 'totk', NOW)
        again = saves.read_backup(backup.directory)
        self.assertEqual(again, backup)

    def test_a_second_backup_at_the_same_second_is_refused(self):
        saves.backup(self.profile, self.backups, 'totk', 'totk', NOW)
        backup, error = saves.backup(self.profile, self.backups, 'totk', 'totk', NOW)
        self.assertIsNone(backup)
        self.assertEqual(error, 'backup_exists')

    def test_a_root_inside_the_source_is_refused(self):
        backup, error = saves.backup(self.profile, self.profile / 'backups', 'totk', 'totk', NOW)
        self.assertIsNone(backup)
        self.assertEqual(error, 'root_inside_source')

    def test_a_missing_source_is_refused(self):
        backup, error = saves.backup(self.root / 'nope', self.backups, 'totk', 'totk', NOW)
        self.assertIsNone(backup)
        self.assertEqual(error, 'source_missing')

    def test_without_hashing_the_manifest_keeps_sizes_only(self):
        backup, _ = saves.backup(self.profile, self.backups, 'totk', 'totk', NOW, with_hash=False)
        self.assertTrue(all(item.sha256 is None for item in backup.files))

    def test_a_manifest_naming_a_path_outside_the_backup_reads_as_none(self):
        for bad in ('../escape', '/etc/passwd', 'a/../../b', 'a//b', '', 'a\\b'):
            folder = self.root / f'bad{abs(hash(bad))}'
            folder.mkdir()
            (folder / saves.BACKUP_JSON).write_text(json.dumps({'format': saves.FORMAT, 'files': [{'path': bad, 'size': 1}]}))
            self.assertIsNone(saves.read_backup(folder), bad)

    def test_a_folder_that_is_not_a_backup_reads_as_none(self):
        (self.root / 'plain').mkdir()
        self.assertIsNone(saves.read_backup(self.root / 'plain'))
        self.assertIsNone(saves.read_backup(self.root / 'nope'))

    def test_finding_backups_orders_newest_first_and_skips_other_folders(self):
        saves.backup(self.profile, self.backups, 'totk', 'totk', NOW)
        saves.backup(self.profile, self.backups, 'totk', 'totk', NOW + 60)
        (self.backups / 'zzz-not-a-backup').mkdir()
        found = saves.find_backups(self.backups)
        self.assertEqual(len(found), 2)
        self.assertGreater(found[0].directory.name, found[1].directory.name)
        self.assertEqual(saves.find_backups(self.root / 'nope'), ())


class Restore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.profile = self.root / 'profile'
        write(self.profile / 'progress.sav', b'old')
        write(self.profile / 'keep.sav', b'keep')
        self.backup, _ = saves.backup(self.profile, self.root / 'backups', 'totk', 'totk', NOW)
        (self.profile / 'progress.sav').write_bytes(b'newer')
        (self.profile / 'keep.sav').unlink()

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_plan_says_what_is_written_and_what_is_left(self):
        write(self.profile / 'later.sav', b'later')
        plan = saves.plan_restore(self.backup, self.profile)
        self.assertEqual([f.path for f in plan.write], ['keep.sav', 'progress.sav'])
        self.assertEqual(plan.kept, ('later.sav',))
        self.assertTrue(plan.target_exists)

    def test_restore_writes_the_backup_and_keeps_a_newer_file(self):
        write(self.profile / 'later.sav', b'later')
        plan, error = saves.restore(self.backup, self.profile)
        self.assertIsNone(error)
        self.assertEqual((self.profile / 'progress.sav').read_bytes(), b'old')
        self.assertEqual((self.profile / 'keep.sav').read_bytes(), b'keep')
        self.assertEqual((self.profile / 'later.sav').read_bytes(), b'later')
        self.assertEqual(plan.kept, ('later.sav',))
        self.assertTrue(self.backup.directory.is_dir())                 # the backup is never removed

    def test_the_same_file_is_not_written_again(self):
        self.profile.joinpath('progress.sav').write_bytes(b'old')
        plan = saves.plan_restore(self.backup, self.profile)
        self.assertIn('progress.sav', [f.path for f in plan.same])

    def test_a_missing_target_is_created(self):
        target = self.root / 'fresh'
        plan, error = saves.restore(self.backup, target)
        self.assertIsNone(error)
        self.assertFalse(plan.target_exists)
        self.assertEqual((target / 'progress.sav').read_bytes(), b'old')


if __name__ == '__main__':
    unittest.main()
