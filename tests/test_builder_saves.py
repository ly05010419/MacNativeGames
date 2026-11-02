"""tools/builder/saves.py (S3): the save folders the builder finds, a backup that keeps the folder shape and a
backup.json, and a restore that writes over the profile without ever deleting a backup or a newer file."""
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest.mock import patch

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


class RestoreIntegrity(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        self.target = self.root / 'target'
        for name in ('a.bin', 'z.bin'):
            write(self.source / name, b'original')
            write(self.target / name, b'changed!')
        self.backup, error = saves.backup(self.source, self.root / 'backups', 'test', 'totk', NOW)
        self.assertIsNone(error)
        for path in [self.target, *self.target.rglob('*')]:
            os.utime(path, ns=(1234567890000000000, 1234567890000000000))

    @staticmethod
    def snapshot(root):
        if not root.exists():
            return None
        result = {}
        for path in [root, *sorted(root.rglob('*'))]:
            info = path.lstat()
            result[str(path.relative_to(root))] = (
                stat.S_IFMT(info.st_mode), info.st_mtime_ns,
                path.read_bytes() if stat.S_ISREG(info.st_mode) else None)
        return result

    def entry(self, name, **changes):
        manifest = self.backup.directory / 'backup.json'
        data = json.loads(manifest.read_text())
        next(item for item in data['files'] if item['path'] == name).update(changes)
        manifest.write_text(json.dumps(data))
        self.backup = saves.read_backup(self.backup.directory)
        self.assertIsNotNone(self.backup)

    def rejected(self, target=None):
        target = self.target if target is None else target
        before = self.snapshot(target)
        result = saves.restore(self.backup, target)
        self.assertIsInstance(result, tuple)
        plan, error = result
        self.assertIsInstance(plan, saves.RestorePlan)
        self.assertIsInstance(error, str)
        self.assertTrue(error.strip())
        self.assertEqual(self.snapshot(target), before)
        return error

    def test_same_size_corruption_is_rejected(self):
        (self.backup.directory / 'a.bin').write_bytes(b'altered!')
        self.assertIn('a.bin', self.rejected())

    def test_all_entries_are_checked_before_the_first_copy(self):
        (self.backup.directory / 'z.bin').write_bytes(b'altered!')
        self.assertIn('z.bin', self.rejected())

    def test_a_missing_backup_file_is_rejected(self):
        (self.backup.directory / 'z.bin').unlink()
        self.rejected()

    def test_a_truncated_backup_file_is_rejected(self):
        (self.backup.directory / 'a.bin').write_bytes(b'x')
        self.rejected()

    def test_an_expanded_backup_file_is_rejected(self):
        (self.backup.directory / 'a.bin').write_bytes(b'original plus')
        self.rejected()

    def test_invalid_backup_does_not_create_target_or_parents(self):
        (self.backup.directory / 'z.bin').unlink()
        target = self.root / 'missing' / 'nested'
        self.rejected(target)
        self.assertFalse(target.parent.exists())

    def test_a_corrupt_same_entry_is_still_rejected(self):
        (self.target / 'a.bin').write_bytes(b'original')
        self.assertIn('a.bin', [f.path for f in saves.plan_restore(self.backup, self.target).same])
        (self.backup.directory / 'a.bin').write_bytes(b'altered!')
        self.rejected()

    def test_a_real_hashless_backup_restores_with_limited_checks(self):
        self.backup, error = saves.backup(self.source, self.root / 'hashless', 'test', 'totk', NOW,
                                          with_hash=False)
        self.assertIsNone(error)
        self.assertTrue(all(f.sha256 is None for f in self.backup.files))
        # Equal-size content changes cannot be detected without a recorded hash.
        (self.backup.directory / 'a.bin').write_bytes(b'altered!')
        _, error = saves.restore(self.backup, self.target)
        self.assertIsNone(error)
        self.assertEqual((self.target / 'a.bin').read_bytes(), b'altered!')

    def test_hashless_size_and_type_are_still_checked(self):
        self.backup, error = saves.backup(self.source, self.root / 'hashless', 'test', 'totk', NOW,
                                          with_hash=False)
        self.assertIsNone(error)
        path = self.backup.directory / 'a.bin'
        path.write_bytes(b'x')
        self.rejected()
        path.unlink()
        path.mkdir()
        self.rejected()

    def test_extra_backup_files_are_ignored_and_target_files_are_kept(self):
        write(self.backup.directory / '.DS_Store', b'finder metadata')
        write(self.target / 'extra.bin', b'keep me')
        plan, error = saves.restore(self.backup, self.target)
        self.assertIsNone(error)
        self.assertIn('extra.bin', plan.kept)
        self.assertEqual((self.target / 'extra.bin').read_bytes(), b'keep me')
        self.assertFalse((self.target / '.DS_Store').exists())

    def test_special_copy_errors_are_never_reported_as_success(self):
        for error in (shutil.Error('copy failed'), shutil.SameFileError('same file'),
                      shutil.SpecialFileError('special file'), shutil.Error(), OSError(),
                      ValueError('invalid copy path')):
            with self.subTest(error=type(error).__name__, message=str(error)):
                with patch.object(saves.shutil, 'copy2', side_effect=error):
                    self.rejected()

    def test_a_copy_failure_after_another_copy_reports_partial_failure(self):
        copy = shutil.copy2

        def fail_last(src, dst):
            if Path(src).name == 'z.bin':
                raise shutil.Error('last copy failed')
            return copy(src, dst)

        with patch.object(saves.shutil, 'copy2', side_effect=fail_last):
            result = saves.restore(self.backup, self.target)
        self.assertIsInstance(result, tuple)
        self.assertEqual(len(result), 2)
        plan, error = result
        self.assertIsInstance(plan, saves.RestorePlan)
        self.assertEqual([f.path for f in plan.write], ['a.bin', 'z.bin'])
        self.assertIsInstance(error, str)
        self.assertTrue(error.strip())
        self.assertIn("Restore of 'z.bin' failed: last copy failed", error)
        self.assertIn('partially updated', error)
        self.assertEqual((self.target / 'a.bin').read_bytes(), b'original')
        self.assertEqual((self.target / 'z.bin').read_bytes(), b'changed!')

    def test_copy_failure_represents_the_entry_path_safely(self):
        name = "z\n'quoted'.bin"
        (self.backup.directory / 'z.bin').rename(self.backup.directory / name)
        self.entry('z.bin', path=name)
        copy = shutil.copy2

        def fail_last(src, dst):
            if Path(src).name == name:
                raise OSError()
            return copy(src, dst)

        with patch.object(saves.shutil, 'copy2', side_effect=fail_last):
            plan, error = saves.restore(self.backup, self.target)
        self.assertIsInstance(plan, saves.RestorePlan)
        self.assertEqual(error, f'Restore of {name!r} failed: OSError; '
                                'destination may be partially updated')
        self.assertNotIn('\n', error)

    def test_initial_target_mkdir_failure_does_not_name_a_backup_entry(self):
        before = self.snapshot(self.target)
        with patch.object(Path, 'mkdir', side_effect=OSError()), \
             patch.object(saves.shutil, 'copy2') as copy:
            result = saves.restore(self.backup, self.target)
        self.assertIsInstance(result, tuple)
        self.assertEqual(len(result), 2)
        plan, error = result
        self.assertIsInstance(plan, saves.RestorePlan)
        self.assertEqual([f.path for f in plan.write], ['a.bin', 'z.bin'])
        self.assertEqual(error, 'OSError; destination may be partially updated')
        self.assertNotIn('a.bin', error)
        self.assertNotIn('z.bin', error)
        copy.assert_not_called()
        self.assertEqual(self.snapshot(self.target), before)

    def test_a_parent_folder_failure_names_the_entry_being_copied(self):
        write(self.source / 'slot1/x.bin', b'nested')
        self.backup, error = saves.backup(
            self.source, self.root / 'nested', 'test', 'totk', NOW)
        self.assertIsNone(error)
        self.assertEqual([f.path for f in self.backup.files],
                         ['a.bin', 'z.bin', 'slot1/x.bin'])
        mkdir = Path.mkdir

        def fail_slot(path, *args, **kwargs):
            if path.name == 'slot1':
                raise OSError()
            return mkdir(path, *args, **kwargs)

        with patch.object(Path, 'mkdir', fail_slot):
            result = saves.restore(self.backup, self.target)
        self.assertIsInstance(result, tuple)
        self.assertEqual(len(result), 2)
        plan, error = result
        self.assertIsInstance(plan, saves.RestorePlan)
        self.assertEqual(error, "Restore of 'slot1/x.bin' failed: OSError; "
                                "destination may be partially updated")
        self.assertEqual((self.target / 'a.bin').read_bytes(), b'original')
        self.assertEqual((self.target / 'z.bin').read_bytes(), b'original')
        self.assertFalse((self.target / 'slot1').exists())

    def test_a_nul_manifest_path_returns_an_error(self):
        self.entry('z.bin', path='bad\0name')
        self.rejected()

    def test_planning_errors_preserve_the_result_contract(self):
        with patch.object(saves, 'plan_restore', side_effect=ValueError('invalid target')):
            self.rejected()

    def test_a_directory_is_not_a_backup_file(self):
        path = self.backup.directory / 'a.bin'
        path.unlink()
        path.mkdir()
        self.rejected()

    @unittest.skipUnless(hasattr(os, 'O_NOFOLLOW'), 'requires no-follow file opens')
    def test_a_final_symlink_is_rejected_even_with_matching_hash(self):
        outside = self.root / 'outside.bin'
        outside.write_bytes(b'original')
        path = self.backup.directory / 'a.bin'
        path.unlink()
        path.symlink_to(outside)
        self.rejected()
        self.assertEqual(outside.read_bytes(), b'original')

    @unittest.skipUnless(hasattr(os, 'mkfifo'), 'requires POSIX FIFOs')
    def test_fifo_rejection_is_bounded_in_a_separate_process(self):
        path = self.backup.directory / 'a.bin'
        path.unlink()
        os.mkfifo(path)
        self.entry('a.bin', size=0, sha256=hashlib.sha256(b'').hexdigest())
        before = self.snapshot(self.target)
        code = ('import json,sys; from tools.builder import saves; '
                'b=saves.read_backup(sys.argv[1]); p,e=saves.restore(b,sys.argv[2]); '
                'print(json.dumps(e))')
        try:
            done = subprocess.run([sys.executable, '-B', '-c', code, str(self.backup.directory),
                                   str(self.target)], cwd=Path(__file__).resolve().parents[1],
                                  capture_output=True, text=True, timeout=10, check=True)
        except subprocess.TimeoutExpired:
            self.fail('FIFO preflight blocked; subprocess.run killed and reaped the child')
        error = json.loads(done.stdout)
        self.assertIsInstance(error, str)
        self.assertTrue(error.strip())
        self.assertEqual(self.snapshot(self.target), before)

    def test_descriptor_errors_close_the_file_and_leave_target_untouched(self):
        real_open, real_close = os.open, os.close
        for failing in ('fstat', 'read'):
            opened, closed = [], []

            def record_open(*args, **kwargs):
                fd = real_open(*args, **kwargs)
                opened.append(fd)
                return fd

            def record_close(fd):
                closed.append(fd)
                real_close(fd)

            with self.subTest(failing=failing):
                with patch.object(saves.os, 'open', side_effect=record_open), \
                     patch.object(saves.os, 'close', side_effect=record_close), \
                     patch.object(saves.os, failing, side_effect=OSError('synthetic read failure')):
                    self.rejected()
                self.assertTrue(opened)
                self.assertEqual(opened, closed)

    def test_open_failure_is_reported_without_writes(self):
        with patch.object(saves.os, 'open', side_effect=PermissionError('synthetic open failure')):
            self.rejected()

    def test_safe_flags_and_descriptor_closure_on_success(self):
        real_open, real_close = os.open, os.close
        opened, closed = [], []

        def record_open(path, flags, *args, **kwargs):
            self.assertEqual(flags & os.O_ACCMODE, os.O_RDONLY)
            self.assertTrue(flags & os.O_NOFOLLOW)
            self.assertTrue(flags & os.O_NONBLOCK)
            if hasattr(os, 'O_CLOEXEC'):
                self.assertTrue(flags & os.O_CLOEXEC)
            fd = real_open(path, flags, *args, **kwargs)
            self.assertFalse(os.get_inheritable(fd))
            opened.append(fd)
            return fd

        def record_close(fd):
            closed.append(fd)
            real_close(fd)

        with patch.object(saves.os, 'open', side_effect=record_open), \
             patch.object(saves.os, 'close', side_effect=record_close):
            _, error = saves.restore(self.backup, self.target)
        self.assertIsNone(error)
        self.assertEqual(len(opened), len(self.backup.files))
        self.assertEqual(opened, closed)

    def test_missing_critical_open_flags_do_not_silently_weaken_checks(self):
        for flag in ('O_NOFOLLOW', 'O_NONBLOCK'):
            with self.subTest(flag=flag), patch.object(saves.os, flag, None):
                self.rejected()

    def test_empty_and_unicode_named_files_are_supported(self):
        write(self.source / 'ñandú save.bin', b'')
        self.backup, error = saves.backup(self.source, self.root / 'unicode', 'test', 'totk', NOW)
        self.assertIsNone(error)
        _, error = saves.restore(self.backup, self.target)
        self.assertIsNone(error)
        self.assertEqual((self.target / 'ñandú save.bin').read_bytes(), b'')

    def test_a_recorded_hash_is_checked_regardless_of_hash_limit(self):
        (self.backup.directory / 'a.bin').write_bytes(b'altered!')
        with patch.object(saves, 'HASH_LIMIT', 1):
            self.rejected()

    def test_an_empty_hash_is_not_treated_as_missing(self):
        self.entry('a.bin', sha256='')
        self.rejected()


if __name__ == '__main__':
    unittest.main()
