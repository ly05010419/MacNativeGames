"""tools/builder/config.py (S1.6): saving (C3). Atomicity, the hard-linked backup of a broken file, its name
collisions, and what is left when each step fails, including during a recovery."""
import errno
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from tools.builder import config
from tools.builder.config import SavedPaths, Settings, SettingsStore

NOON = time.mktime((2026, 10, 23, 12, 0, 0, 0, 0, -1))          # local time, as the store formats it
BACKUP = 'settings.json.broken-20261023-120000'
BROKEN = b'{"format": 1, "games": {"totk": {"game": "/my/precious/path.xci"'
NEW = Settings(paths={'totk': SavedPaths(game='/new.xci')})


class Clock:
    def monotonic(self):
        return 0.0

    def wall(self):
        return NOON


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.folder = Path(tmp.name) / 'MacNativeGames' / 'Builder'
        self.path = self.folder / 'settings.json'
        self.store = SettingsStore(self.path, Clock())

    def listing(self):
        return sorted(p.name for p in self.folder.iterdir())

    def broken(self):
        """A broken settings.json, loaded so the store knows it is broken."""
        self.folder.mkdir(parents=True)
        self.path.write_bytes(BROKEN)
        self.assertEqual(self.store.load().problems, ('invalid_json',))


class Saving(Base):
    def test_folder_created_and_file_written(self):
        self.store.save(NEW)
        self.assertEqual(self.listing(), ['settings.json'])
        self.assertEqual(json.loads(self.path.read_text())['games']['totk']['game'], '/new.xci')

    def test_replace_failure_keeps_the_old_file_and_no_temporary(self):
        self.store.save(Settings(paths={'botw': SavedPaths(game='/old.xci')}))
        before = self.path.read_bytes()
        with patch.object(config.os, 'replace', side_effect=OSError(errno.EIO, 'I/O error')):
            self.assertRaises(OSError, self.store.save, NEW)
        self.assertEqual((self.path.read_bytes(), self.listing()), (before, ['settings.json']))

    def test_failure_halfway_through_writing(self):
        self.store.save(Settings(paths={'botw': SavedPaths(game='/old.xci')}))
        before = self.path.read_bytes()
        def half(obj, fp, **kw):
            fp.write('{"format": 1, "gam')
            raise OSError(errno.ENOSPC, 'No space left on device')
        with patch.object(config.json, 'dump', side_effect=half):
            self.assertRaises(OSError, self.store.save, NEW)
        self.assertEqual((self.path.read_bytes(), self.listing()), (before, ['settings.json']))

    def test_fsync_before_replace(self):
        order = []
        real_fsync, real_replace = os.fsync, os.replace
        with patch.object(config.os, 'fsync', side_effect=lambda fd: order.append('fsync') or real_fsync(fd)), \
             patch.object(config.os, 'replace', side_effect=lambda a, b: order.append('replace') or real_replace(a, b)):
            self.store.save(NEW)
        self.assertEqual(order, ['fsync', 'replace'])

    def test_nothing_secret_is_stored(self):
        keys = self.folder.parent / 'prod.keys'
        keys.parent.mkdir(parents=True)
        keys.write_text('header_key = SECRET-MARKER-123\n')
        self.store.save(Settings(paths={'totk': SavedPaths(game='/g.xci', keys=str(keys))}))
        self.assertIn(str(keys), self.path.read_text())
        self.assertNotIn('SECRET-MARKER-123', self.path.read_text())


class BrokenFile(Base):
    def test_backup_kept_and_made_once(self):
        self.broken()
        self.store.save(NEW)
        self.assertEqual((self.folder / BACKUP).read_bytes(), BROKEN)
        self.assertEqual(json.loads(self.path.read_text())['games']['totk']['game'], '/new.xci')
        self.store.save(NEW)
        self.assertEqual(self.listing(), ['settings.json', BACKUP])

    def test_content_problem_makes_no_backup(self):
        self.folder.mkdir(parents=True)
        self.path.write_text(json.dumps({'format': 'x'}))
        self.assertEqual(self.store.load().problems, ('format_invalid',))
        self.store.save(NEW)
        self.assertEqual(self.listing(), ['settings.json'])

    def test_name_collision_never_overwrites(self):
        self.broken()
        (self.folder / BACKUP).write_bytes(b'an earlier backup')
        self.store.save(NEW)
        self.assertEqual((self.folder / BACKUP).read_bytes(), b'an earlier backup')
        self.assertEqual((self.folder / f'{BACKUP}-2').read_bytes(), BROKEN)

    def test_all_names_taken(self):
        self.broken()
        for name in [BACKUP] + [f'{BACKUP}-{n}' for n in range(2, config.BACKUP_NAMES + 1)]:
            (self.folder / name).write_bytes(b'taken')
        with self.assertRaises(FileExistsError):
            self.store.save(NEW)
        self.assertEqual(self.path.read_bytes(), BROKEN)
        self.assertFalse([n for n in self.listing() if n.endswith('.tmp')])


class FailuresDuringRecovery(Base):
    """One test per row of the C3 table, with a broken file loaded first."""

    def assertLeft(self, backups):
        self.assertEqual(self.path.read_bytes(), BROKEN)                       # never missing, never half
        self.assertEqual([n for n in self.listing() if n.startswith('settings.json.broken')], backups)
        self.assertFalse([n for n in self.listing() if n.endswith('.tmp')])

    def test_fsync_fails(self):
        self.broken()
        with patch.object(config.os, 'fsync', side_effect=OSError(errno.EIO, 'I/O error')):
            self.assertRaises(OSError, self.store.save, NEW)
        self.assertLeft([])

    def test_link_fails(self):
        self.broken()
        with patch.object(config.os, 'link', side_effect=PermissionError(errno.EPERM, 'Operation not permitted')):
            self.assertRaises(PermissionError, self.store.save, NEW)
        self.assertLeft([])

    def test_replace_fails_then_a_real_save_makes_no_second_backup(self):
        self.broken()
        with patch.object(config.os, 'replace', side_effect=OSError(errno.EIO, 'I/O error')):
            self.assertRaises(OSError, self.store.save, NEW)
        self.assertLeft([BACKUP])
        self.assertEqual((self.folder / BACKUP).read_bytes(), BROKEN)
        self.store.save(NEW)
        self.assertEqual(self.listing(), ['settings.json', BACKUP])
        self.assertEqual(json.loads(self.path.read_text())['games']['totk']['game'], '/new.xci')


if __name__ == '__main__':
    unittest.main()
