"""tools/builder/storage.py (S2.2): where each cache is, what it holds, and the guards that keep a clean or a move
away from a root, the home folder, a system folder and the game, save or app a caller protects."""
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from tools.builder import storage
from tools.builder.games import GAMES


def write(path, data=b'x'):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class WorkFolder(unittest.TestCase):
    def test_the_saved_work_path_wins(self):
        folder = storage.work_dir(GAMES['totk'], '/games/Tears.xci', '/elsewhere/TOTK_build')
        self.assertEqual(folder, Path('/elsewhere/TOTK_build'))

    def test_without_a_saved_path_it_sits_beside_the_game_file(self):
        folder = storage.work_dir(GAMES['totk'], '/games/Tears.xci', None)
        self.assertEqual(folder, Path('/games/TOTK_build'))
        self.assertEqual(storage.work_dir(GAMES['botw'], '/games/BotW.wux', ''),
                         Path('/games/BotW_build'))

    def test_without_a_game_file_there_is_no_work_folder(self):
        self.assertIsNone(storage.work_dir(GAMES['totk'], None, None))


class ShaderFolder(unittest.TestCase):
    def test_the_default_is_the_launchers_application_support_folder(self):
        home = Path('/Users/me')
        self.assertEqual(storage.shader_dir(GAMES['totk'], home, {}),
                         home / 'Library/Application Support/MacNativeGames/TOTK/shader-cache')
        self.assertEqual(storage.shader_dir(GAMES['botw'], home, {}),
                         home / 'Library/Application Support/MacNativeGames/BotW/shader-cache')

    def test_the_environment_moves_it_and_botw_reads_the_totk_name(self):
        self.assertEqual(storage.shader_dir(GAMES['totk'], '/Users/me', {'TOTK_SHADER_CACHE': '/c/totk'}),
                         Path('/c/totk'))
        self.assertEqual(storage.shader_dir(GAMES['botw'], '/Users/me', {'BOTW_SHADER_CACHE': '/c/botw'}),
                         Path('/Users/me/Library/Application Support/MacNativeGames/BotW/shader-cache'))   # no such name
        self.assertEqual(storage.shader_dir(GAMES['botw'], '/Users/me', {'TOTK_SHADER_CACHE': '/c/totk'}),
                         Path('/c/totk'))

    def test_the_work_variable_is_the_one_the_window_names(self):
        self.assertEqual(storage.shader_env(GAMES['totk']), ('TOTK_SHADER_CACHE', ('TOTK_SHADER_CACHE',)))
        self.assertEqual(storage.shader_env(GAMES['botw']), ('TOTK_SHADER_CACHE', ('TOTK_SHADER_CACHE',)))   # one launcher


class Measure(unittest.TestCase):
    def test_a_folder_counts_its_files_and_bytes_recursively(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            write(root / 'a.bin', b'1234')
            write(root / 'nested/b.bin', b'12')
            size = storage.measure(root)
        self.assertEqual((size.bytes, size.files, size.errors), (6, 2, ()))
        self.assertTrue(size.clean)

    def test_a_missing_folder_measures_as_empty(self):
        with tempfile.TemporaryDirectory() as folder:
            size = storage.measure(Path(folder) / 'nope')
        self.assertEqual((size.bytes, size.files), (0, 0))

    def test_sizes_read_as_a_person_reads_them(self):
        self.assertEqual(storage.human(0), '0 B')
        self.assertEqual(storage.human(512), '512 B')
        self.assertEqual(storage.human(1536), '1.5 KB')
        self.assertEqual(storage.human(2 ** 30), '1.0 GB')


class Guards(unittest.TestCase):
    def setUp(self):
        # The temporary folder lives under /var on macOS; the system-name rule is tested against a real
        # system folder in SystemFolders below, so a plain cache inside the temporary home is allowed here.
        self.patch = mock.patch.object(storage, 'SYSTEM', frozenset())
        self.patch.start()
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name) / 'home'
        self.cache = self.home / 'Library/Application Support/MacNativeGames/TOTK/shader-cache'
        self.cache.mkdir(parents=True)
        write(self.cache / 'rt-x/pipeline.bin', b'abc')

    def tearDown(self):
        self.tmp.cleanup()
        self.patch.stop()

    def test_the_home_folder_and_its_parents_are_refused(self):
        self.assertEqual(storage.refuse_reason(self.home, self.home), 'home')
        self.assertEqual(storage.refuse_reason(Path(self.tmp.name), self.home), 'home')

    def test_a_missing_folder_a_file_and_a_root_are_refused(self):
        self.assertEqual(storage.refuse_reason(self.home / 'nope', self.home), 'missing')
        self.assertEqual(storage.refuse_reason(self.cache / 'rt-x/pipeline.bin', self.home), 'file')
        self.assertEqual(storage.refuse_reason(Path('/'), self.home), 'root')

    def test_a_protected_path_refuses_the_folder_above_it(self):
        game = self.cache / 'rt-x/pipeline.bin'
        self.assertEqual(storage.refuse_reason(self.cache, self.home, (game,)), 'protected')
        self.assertEqual(storage.refuse_reason(self.cache / 'rt-x', self.home, (game,)), 'protected')

    def test_a_plain_cache_folder_is_allowed(self):
        self.assertIsNone(storage.refuse_reason(self.cache, self.home))


class SystemFolders(unittest.TestCase):
    def test_a_system_folder_is_refused(self):
        if not Path('/System').is_dir():
            self.skipTest('no /System on this host')
        self.assertEqual(storage.refuse_reason(Path('/System'), Path.home()), 'system')


class Clean(unittest.TestCase):
    def setUp(self):
        self.patch = mock.patch.object(storage, 'SYSTEM', frozenset())
        self.patch.start()
        self.tmp = tempfile.TemporaryDirectory()
        self.home = Path(self.tmp.name) / 'home'
        self.cache = self.home / 'cache'
        write(self.cache / 'one.bin', b'12')
        write(self.cache / 'deep/two.bin', b'345')

    def tearDown(self):
        self.tmp.cleanup()
        self.patch.stop()

    def test_cleaning_empties_the_folder_and_keeps_it(self):
        removed, error = storage.clean(self.cache, self.home)
        self.assertIsNone(error)
        self.assertEqual((removed.bytes, removed.files), (5, 2))
        self.assertTrue(self.cache.is_dir())
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_a_refused_folder_is_untouched_and_says_why(self):
        removed, error = storage.clean(self.home, self.home)
        self.assertEqual(removed, storage.Size())
        self.assertEqual(error, 'home')
        self.assertTrue((self.cache / 'one.bin').is_file())


class Move(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'source'
        write(self.source / 'one.bin', b'12')
        write(self.source / 'deep/two.bin', b'345')

    def tearDown(self):
        self.tmp.cleanup()

    def test_the_contents_move_into_an_empty_target(self):
        target = self.root / 'target'
        moved, error = storage.move(self.source, target)
        self.assertEqual(moved, 2)
        self.assertIsNone(error)
        self.assertTrue((target / 'one.bin').is_file())
        self.assertTrue((target / 'deep/two.bin').is_file())
        self.assertEqual(list(self.source.iterdir()), [])

    def test_a_target_that_is_not_empty_is_refused(self):
        target = self.root / 'target'
        write(target / 'keep.bin', b'k')
        moved, error = storage.move(self.source, target)
        self.assertEqual((moved, error), (0, 'target_not_empty'))
        self.assertTrue((self.source / 'one.bin').is_file())

    def test_a_target_inside_the_source_is_refused(self):
        moved, error = storage.move(self.source, self.source / 'target')
        self.assertEqual((moved, error), (0, 'inside_source'))
        self.assertTrue((self.source / 'one.bin').is_file())

    def test_a_missing_source_is_refused(self):
        moved, error = storage.move(self.root / 'nope', self.root / 'target')
        self.assertEqual((moved, error), (0, 'source_missing'))


if __name__ == '__main__':
    unittest.main()
