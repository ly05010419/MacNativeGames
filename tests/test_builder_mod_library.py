"""tools/builder/mod_library.py (S5.2): the library, the toggles and order, Apply through main's real
mod_folder.install, the marker and which backups may go, and the first-use import, against a fake built app.

O6: HOME points into a temporary folder before anything runs, and setUp fails unless main's mods_folder() resolves
inside it, so no test can touch the real ~/Library/Application Support/MacNativeGames."""
import fcntl
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from test_mod_manifest import fake_romfs
from tools.builder import mod_add, mod_library, mod_tools
from tools.builder.games import TOTK
from tools.builder.ports import CommandRunner, RealClock

REPO = Path(__file__).resolve().parents[1]
FILES = {'Pack/A.pack.zs': b'original a', 'Sound/B.bwav': b'original b' * 100, 'UI/C.bfres': b'c'}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()
        home = patch.dict(os.environ, HOME=str(self.tmp / 'home'))
        home.start()
        self.addCleanup(home.stop)
        _, self.folder_tool = mod_tools.tools(REPO)
        self.load = Path(self.folder_tool.mods_folder('v143'))
        self.assertTrue(self.load.is_relative_to(self.tmp), 'the mod folder must resolve inside the test folder')
        self.data = self.tmp / 'home/Library/Application Support/MacNativeGames'
        self.app = self.tmp / 'Tears of the Kingdom.app'
        fake_romfs(self.app / 'Contents/Resources/totk/extracted/v143/romfs', FILES)
        self.runner = CommandRunner(RealClock())
        self.library = mod_library.open_library(self.data / 'TOTK', 'v143', self.load)

    def source(self, name, files):
        root = self.tmp / 'mods' / name
        for path, data in files.items():
            (root / 'romfs' / path).parent.mkdir(parents=True, exist_ok=True)
            (root / 'romfs' / path).write_bytes(data)
        return root

    def add(self, name, files):
        [mod_id] = mod_add.add(self.runner, REPO, self.app, 'v143', self.library, self.source(name, files))
        return mod_id

    def apply(self):
        return mod_library.apply(REPO, TOTK, self.app, 'v143', self.library, self.data)


class Library(Base):
    def test_add_clones_and_validates(self):
        mod_id = self.add('Better Grass', {'Pack/A.pack.zs': b'modded a'})
        copy = self.library.path(mod_id) / 'romfs/Pack/A.pack.zs'
        original = self.tmp / 'mods/Better Grass/romfs/Pack/A.pack.zs'
        self.assertEqual((copy.read_bytes(), mod_library.validation(self.library, mod_id).valid), (b'modded a', True))
        self.assertNotEqual(copy.stat().st_ino, original.stat().st_ino)
        reopened = mod_library.open_library(self.data / 'TOTK', 'v143', self.load)
        self.assertEqual(reopened.state['names'][mod_id], 'Better Grass')

    def test_a_folder_without_romfs_or_exefs_is_refused(self):
        empty = self.tmp / 'mods/empty'
        empty.mkdir(parents=True)
        self.assertRaises(ValueError, mod_add.add, self.runner, REPO, self.app, 'v143', self.library, empty)

    def test_symbolic_links_are_refused(self):
        source = self.source('linked', {'Pack/A.pack.zs': b'x'})
        (source / 'romfs/Elsewhere').symlink_to(self.tmp)
        self.assertRaises(ValueError, mod_add.add, self.runner, REPO, self.app, 'v143', self.library, source)

    def test_a_failed_copy_leaves_nothing_in_the_library(self):
        with patch.object(mod_library, 'clone', side_effect=OSError('disk full')):
            self.assertRaises(OSError, self.add, 'A', {'Pack/A.pack.zs': b'x'})
        self.assertEqual(list(self.library.root.glob('*')) if self.library.root.exists() else [], [])

    def test_only_a_valid_mod_can_be_enabled(self):
        bad = self.add('one level off', {'romfs/Pack/A.pack.zs': b'x'})
        self.assertRaises(ValueError, mod_library.set_enabled, self.library, bad, True)

    def test_order_conflicts_and_remove(self):
        a = self.add('A', {'Pack/A.pack.zs': b'from a'})
        b = self.add('B', {'Pack/A.pack.zs': b'from b', 'Sound/B.bwav': b'from b'})
        for mod_id in (a, b):
            mod_library.set_enabled(self.library, mod_id, True)
        listed = {m.id: m for m in mod_library.mods(self.library)}
        self.assertEqual((listed[a].conflicts, listed[b].conflicts), (('Pack/A.pack.zs',),) * 2)
        mod_library.move(self.library, b, -1)
        self.assertEqual(self.library.state['order'], [b, a])
        mod_library.remove(self.library, b)
        self.assertEqual((self.library.state['order'], self.library.path(b).exists()), ([a], False))
        self.assertTrue((self.tmp / 'mods/B/romfs/Pack/A.pack.zs').exists())      # the player's folder stays

    def test_the_same_bytes_in_two_mods_are_no_conflict_once_read_in_full(self):      # mod_shared, 2026-10-28
        same, other = {'Pack/A.pack.zs': b'shared'}, {'Pack/A.pack.zs': b'shares'}    # equal sizes, other bytes
        ids = [self.add(name, files) for name, files in (('A', same), ('Copy of A', same), ('C', other))]
        for mod_id in ids[:2]:
            mod_library.set_enabled(self.library, mod_id, True)
        conflicts = lambda: [m.conflicts for m in mod_library.mods(self.library) if m.enabled]
        self.assertEqual(conflicts(), [('Pack/A.pack.zs',)] * 2)          # the quick read cannot know: shown
        mod_library.record_same(self.library)
        self.assertEqual(conflicts(), [(), ()])
        self.assertEqual(mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state['same'],
                         self.library.state['same'])                     # kept in library.json
        mod_library.set_enabled(self.library, ids[2], True)               # other bytes: a conflict for all three
        mod_library.record_same(self.library)
        self.assertEqual(conflicts(), [('Pack/A.pack.zs',)] * 3)
        mod_library.set_enabled(self.library, ids[2], False)
        mod_library.record_same(self.library)
        copy = self.library.path(ids[1]) / 'romfs/Pack/A.pack.zs'
        copy.write_bytes(b'change')                                      # changed since: shown until a full read
        os.utime(copy, ns=(1, 1))
        self.assertEqual(conflicts(), [('Pack/A.pack.zs',)] * 2)
        mod_library.record_same(self.library)
        self.assertEqual(conflicts(), [('Pack/A.pack.zs',)] * 2)

    def test_keys_another_builder_wrote_are_kept(self):
        self.library.state['from_a_fork'] = {'x': [1]}
        mod_library.save(self.library)
        reopened = mod_library.open_library(self.data / 'TOTK', 'v143', self.load)
        self.assertEqual(reopened.state['from_a_fork'], {'x': [1]})
        mod_library.save(reopened)                                        # saved again by a builder that ignores it
        self.assertEqual(mod_library.open_library(self.data / 'TOTK', 'v143', self.load).state['from_a_fork'],
                         {'x': [1]})


class Apply(Base):
    def test_the_later_mod_wins_and_the_folder_is_marked(self):
        a = self.add('A', {'Pack/A.pack.zs': b'from a'})
        b = self.add('B', {'Pack/A.pack.zs': b'from b'})
        for mod_id in (a, b):
            mod_library.set_enabled(self.library, mod_id, True)
        result = self.apply()
        self.assertTrue(result.ok, result)
        self.assertEqual((self.load / 'Pack/A.pack.zs').read_bytes(), b'from b')
        self.assertTrue((self.load / 'manifest.tsv').is_file() and (self.load / mod_library.MARKER).is_file())

    def test_nothing_enabled_sets_the_folder_aside(self):
        mod_library.set_enabled(self.library, self.add('A', {'Pack/A.pack.zs': b'from a'}), True)
        self.apply()
        mod_library.set_enabled(self.library, self.library.state['order'][0], False)
        self.assertEqual(self.apply().reason, 'nothing')
        self.assertFalse(self.load.exists())
        self.assertEqual(len(list(self.load.parent.glob('v143.backup-*'))), 1)        # kept, not deleted

    def test_install_loading_nothing_sets_the_folder_aside(self):                  # O1
        mod_library.set_enabled(self.library, self.add('A', {'Pack/A.pack.zs': b'from a'}), True)
        self.apply()
        with patch.object(self.folder_tool, 'install', return_value=None):
            self.assertEqual(self.apply().reason, 'set_aside')
        self.assertFalse(self.load.exists())

    def test_old_builder_backups_go_and_a_touched_one_stays(self):                 # F3
        mod_library.set_enabled(self.library, self.add('A', {'Pack/A.pack.zs': b'from a'}), True)
        for _ in range(2):
            self.assertTrue(self.apply().ok)
        (self.load / 'Pack/mine.txt').write_text('copied by hand')                 # the player touches it
        for _ in range(2):
            self.assertTrue(self.apply().ok)
        builder_made, theirs = mod_library.backups(self.library)
        self.assertEqual(len(builder_made), 1)                                     # only the newest builder one
        self.assertEqual(len(theirs), 1)
        self.assertTrue((theirs[0] / 'Pack/mine.txt').is_file())

    def test_refused_while_the_game_runs(self):
        mod_library.set_enabled(self.library, self.add('A', {'Pack/A.pack.zs': b'from a'}), True)
        slot = self.data / 'TOTK/game-slot'
        slot.mkdir(parents=True)
        (slot / 'lock').write_text('')
        (slot / 'holder').write_text(f'owner=user\ngame_pid={os.getpid()}\n')
        fd = os.open(slot / 'lock', os.O_RDONLY)
        self.addCleanup(os.close, fd)
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        self.assertEqual(self.apply().reason, 'running')
        self.assertFalse(self.load.exists())


class FirstUse(Base):
    def test_a_folder_copied_by_hand_becomes_one_enabled_mod(self):
        (self.load / 'Pack').mkdir(parents=True)
        (self.load / 'Pack/A.pack.zs').write_bytes(b'by hand')
        (self.load / 'manifest.tsv').write_text('# old\n')
        self.assertTrue(mod_library.import_existing(REPO, self.app, 'v143', self.library))
        mod_id = mod_library.IMPORTED
        self.assertEqual((self.library.state['enabled'], self.library.state['names'][mod_id]),
                         ([mod_id], 'Installed before the builder'))
        self.assertFalse((self.library.path(mod_id) / 'romfs/manifest.tsv').exists())
        self.assertFalse(mod_library.import_existing(REPO, self.app, 'v143', self.library))   # once

    def test_the_players_backups_go_only_when_named(self):
        (self.load.parent / 'v143.backup-1').mkdir(parents=True)
        (self.load.parent / 'v143.backup-2').mkdir()
        self.assertEqual(mod_library.remove_backups(self.library, ('v143.backup-1',)), 1)
        self.assertEqual(sorted(p.name for p in self.load.parent.iterdir()), ['v143.backup-2'])


if __name__ == '__main__':
    unittest.main()
