"""Save migration tests use synthetic bytes, never real player saves."""
import fcntl
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games/totk/scripts'))
import save_manager as saves


@unittest.skipUnless(sys.platform == 'darwin', 'profile exchange requires macOS')
class SaveMigration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'emulator'
        slot = self.source / 'slot_00'
        slot.mkdir(parents=True)
        (slot / 'progress.sav').write_bytes(b'synthetic progress')
        (slot / 'caption.sav').write_bytes(b'synthetic caption')
        (self.source / 'option.sav').write_bytes(b'synthetic options')
        self.profile = self.root / 'profile'
        self.profile.mkdir()
        self.uid = struct.pack('<QQ', 0x1234, 0x5678)
        (self.profile / 'account.uid').write_bytes(self.uid)
        self.original = self.profile / 'save' / saves.account_folder(self.profile)
        self.original.mkdir(parents=True)
        (self.original / 'old.sav').write_bytes(b'old save')
        (self.profile / 'unrelated-setting.txt').write_text('keep')
        self.slot = self.root / 'game-slot'

    def test_import_backs_up_profile_preserves_identity_and_source(self):
        report = saves.import_save(self.source, self.profile, self.slot)
        backup = Path(report['backup'])
        self.assertEqual((self.profile / 'account.uid').read_bytes(), self.uid)
        self.assertEqual((backup / 'account.uid').read_bytes(), self.uid)
        old = backup / 'save' / saves.account_folder(backup) / 'old.sav'
        self.assertEqual(old.read_bytes(), b'old save')
        self.assertFalse((self.original / 'old.sav').exists())
        self.assertEqual((self.profile / 'unrelated-setting.txt').read_text(), 'keep')
        self.assertEqual((self.original / 'slot_00/progress.sav').read_bytes(), b'synthetic progress')
        self.assertEqual((self.source / 'slot_00/progress.sav').read_bytes(), b'synthetic progress')

    def test_export_round_trip_copies_every_file(self):
        saves.import_save(self.source, self.profile, self.slot)
        export = self.root / 'export'
        saves.export_save(export, self.profile, self.slot)
        for path in self.source.rglob('*'):
            if path.is_file():
                self.assertEqual(path.read_bytes(), (export / path.relative_to(self.source)).read_bytes())
        self.assertFalse((export / 'account.uid').exists())
        with self.assertRaisesRegex(ValueError, 'new directory'):
            saves.export_save(export, self.profile, self.slot)

    def test_new_profile_gets_valid_identity(self):
        profile = self.root / 'new/profile'
        report = saves.import_save(self.source, profile, self.slot)
        self.assertIsNone(report['backup'])
        self.assertEqual(len((profile / 'account.uid').read_bytes()), 16)
        self.assertEqual(len(saves.account_folder(profile)), 32)
        self.assertTrue((Path(report['destination']) / 'slot_00/progress.sav').is_file())

    def test_dry_run_creates_nothing(self):
        profile = self.root / 'absent/profile'
        saves.import_save(self.source, profile, self.slot, dry_run=True)
        self.assertFalse(profile.parent.exists())
        self.assertFalse(self.slot.exists())

    def test_busy_game_refuses_import_and_export(self):
        saves.import_save(self.source, self.profile, self.slot)
        with (self.slot / 'lock').open('r+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            for operation, folder in ((saves.import_save, self.source), (saves.export_save, self.root / 'out')):
                with self.assertRaisesRegex(ValueError, 'game is running'):
                    operation(folder, self.profile, self.slot)
        self.assertFalse((self.root / 'out').exists())

    def test_incomplete_source_does_not_touch_profile(self):
        (self.source / 'slot_00/caption.sav').unlink()
        with self.assertRaisesRegex(ValueError, 'Incomplete'):
            saves.import_save(self.source, self.profile, self.slot)
        self.assertEqual((self.original / 'old.sav').read_bytes(), b'old save')
        self.assertFalse(self.slot.exists())

    def test_source_root_and_nested_symlinks_are_refused(self):
        link = self.root / 'linked-source'
        link.symlink_to(self.source, target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'links'):
            saves.import_save(link, self.profile, self.slot)
        (self.source / 'link').symlink_to(self.root / 'profile/account.uid')
        with self.assertRaisesRegex(ValueError, 'Link'):
            saves.import_save(self.source, self.profile, self.slot)

    def test_corrupt_account_is_not_replaced(self):
        (self.profile / 'account.uid').write_bytes(b'bad')
        with self.assertRaisesRegex(ValueError, 'account.uid'):
            saves.import_save(self.source, self.profile, self.slot)
        self.assertEqual((self.profile / 'account.uid').read_bytes(), b'bad')

    def test_overlapping_paths_are_refused(self):
        with self.assertRaisesRegex(ValueError, 'overlap'):
            saves.import_save(self.source, self.source / 'profile', self.slot)

    def test_failed_swap_restores_entire_profile(self):
        with mock.patch.object(saves, 'exchange_directories', side_effect=OSError('simulated swap failure')):
            with self.assertRaisesRegex(OSError, 'swap failure'):
                saves.import_save(self.source, self.profile, self.slot)
        self.assertEqual((self.original / 'old.sav').read_bytes(), b'old save')
        self.assertEqual((self.profile / 'account.uid').read_bytes(), self.uid)
        self.assertFalse(list(self.root.glob('.totk-save-import-*')))
        backups = list(self.root.glob('profile-before-import-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual((backups[0] / 'account.uid').read_bytes(), self.uid)

    def test_lock_inside_a_copied_tree_is_rejected_before_writing(self):
        for lock_dir in (self.profile / 'slot', self.source / 'slot', self.root / 'export/slot'):
            operation = saves.export_save if lock_dir == self.root / 'export/slot' else saves.import_save
            folder = self.root / 'export' if operation is saves.export_save else self.source
            with self.subTest(lock_dir=lock_dir):
                with self.assertRaisesRegex(ValueError, 'lock must be outside'):
                    operation(folder, self.profile, lock_dir)
                self.assertFalse(lock_dir.exists())
        self.assertEqual((self.original / 'old.sav').read_bytes(), b'old save')

    def test_profile_never_disappears_during_replacement(self):
        exchange = saves.exchange_directories
        def check_swap(first, second):
            self.assertTrue(second.is_dir())
            self.assertEqual((second / 'account.uid').read_bytes(), self.uid)
            result = exchange(first, second)
            self.assertTrue(second.is_dir())
            self.assertEqual((second / 'account.uid').read_bytes(), self.uid)
            self.assertEqual((first / 'save' / saves.account_folder(first) / 'old.sav').read_bytes(), b'old save')
            return result
        with mock.patch.object(saves, 'exchange_directories', check_swap):
            saves.import_save(self.source, self.profile, self.slot)

    def test_changed_source_cancels_before_profile_swap(self):
        copytree = saves.shutil.copytree
        def changing_copy(source, destination, *args, **kwargs):
            result = copytree(source, destination, *args, **kwargs)
            if Path(source) == self.source.resolve():
                (self.source / 'option.sav').write_bytes(b'changed while copying')
            return result
        with mock.patch.object(saves.shutil, 'copytree', changing_copy):
            with self.assertRaisesRegex(ValueError, 'changed'):
                saves.import_save(self.source, self.profile, self.slot)
        self.assertEqual((self.original / 'old.sav').read_bytes(), b'old save')
        self.assertFalse(list(self.root.glob('profile-before-import-*')))


if __name__ == '__main__':
    unittest.main()
