"""build_app.py --mac-game-data library: the RomFS image in <data>/GameData/<version> (or the folder --mac-game-data-folder
names) instead of the app, put there once per image, with romfs.sha256 written last (the launcher, app/launcher.m, takes no image without it). A few bytes stand in for
the 16 GB image."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import build_app

SHA = build_app.VERSIONS['botw190']['romfs']['sha256']


class LibraryImage(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        self.tree, self.home = root / 'tree', root / 'home'
        self.image = self.tree / 'extracted/botw190/romfs/romfs.bin'
        self.image.parent.mkdir(parents=True)
        self.image.write_bytes(b'romfs image')
        home = patch.object(build_app.Path, 'home', return_value=self.home)
        home.start()
        self.addCleanup(home.stop)
        self.folder = self.home / 'Library/Application Support/MacNativeGames/BotW/GameData/botw190'

    def place(self):
        return build_app.place_library_image(self.tree, 'botw', 'botw190')

    def test_the_folder_the_launcher_looks_in(self):
        self.assertEqual(build_app.library_game_data('botw', 'botw190'), self.folder)
        self.assertEqual(build_app.library_game_data('totk', 'v143'),
                         self.home / 'Library/Application Support/MacNativeGames/TOTK/GameData/v143')

    def test_put_there_once_and_marked_last(self):
        self.assertTrue(self.place())
        self.assertEqual((self.folder / 'romfs.bin').read_bytes(), b'romfs image')
        self.assertEqual((self.folder / 'romfs.sha256').read_text(), SHA + '\n')
        self.assertEqual(sorted(p.name for p in self.folder.iterdir()), ['romfs.bin', 'romfs.sha256'])
        self.assertFalse(self.place())   # the same image: left as it is

    def test_an_unmarked_or_other_image_is_put_there_again(self):
        self.place()
        (self.folder / 'romfs.sha256').unlink()   # a copy that did not finish
        self.assertTrue(self.place())
        (self.folder / 'romfs.bin').write_bytes(b'short')
        self.assertTrue(self.place())
        (self.folder / 'romfs.sha256').write_text('0' * 64 + '\n')   # another version's image
        self.assertTrue(self.place())
        self.assertEqual((self.folder / 'romfs.bin').read_bytes(), b'romfs image')
        self.assertEqual((self.folder / 'romfs.sha256').read_text(), SHA + '\n')

    def test_a_chosen_folder_holds_each_version_in_its_own_folder(self):   # --mac-game-data-folder
        disk = self.home.parent / 'disk/Games'
        self.assertTrue(build_app.place_library_image(self.tree, 'botw', 'botw190', disk))
        self.assertEqual((disk / 'botw190/romfs.bin').read_bytes(), b'romfs image')
        self.assertEqual((disk / 'botw190/romfs.sha256').read_text(), SHA + '\n')
        self.assertFalse(self.folder.exists())
        self.assertFalse(build_app.place_library_image(self.tree, 'botw', 'botw190', disk))

    def test_a_linked_game_data_folder_holds_the_image(self):   # GameData on another disk
        disk = self.home.parent / 'disk/GameData'
        disk.mkdir(parents=True)
        self.folder.parent.parent.mkdir(parents=True)
        self.folder.parent.symlink_to(disk)
        self.place()
        self.assertEqual((disk / 'botw190/romfs.bin').read_bytes(), b'romfs image')


class Option(unittest.TestCase):
    def test_app_is_the_default(self):
        self.assertEqual(build_app.parse_args(['g.nsp', '-k', 'k']).mac_game_data, 'app')
        self.assertEqual(build_app.parse_args(['g.nsp', '-k', 'k', '--mac-game-data', 'library']).mac_game_data, 'library')

    def resolve(self, *argv, device=False):
        return build_app.mac_game_data(build_app.parse_args(['g.nsp', '-k', 'k', *argv]), device)

    def test_naming_a_folder_is_enough(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.resolve(), ('app', None))
            self.assertEqual(self.resolve('--mac-game-data', 'library'), ('library', None))
            self.assertEqual(self.resolve('--mac-game-data-folder', f'{tmp}/Games'), ('library', Path(tmp).resolve() / 'Games'))

    def test_refused_for_a_missing_disk_and_for_the_phone(self):
        with self.assertRaisesRegex(build_app.BuildError, 'not connected'):
            self.resolve('--mac-game-data-folder', '/Volumes/no such disk/Games')
        with self.assertRaisesRegex(build_app.BuildError, '--ios-game-data documents'):
            self.resolve('--mac-game-data', 'library', device=True)

    def test_launcher_reads_the_same_folder_and_mark(self):
        source = (Path(build_app.__file__).parent / 'games/totk/app/launcher.m').read_text()
        self.assertIn('stringByAppendingPathComponent:@"GameData"]', source)
        self.assertIn('@"romfs.sha256"', source)
        self.assertIn('setenv("TOTK_ROMFS_DATA"', source)
        self.assertIn('@"MNGGameDataFolder"', source)
        self.assertIn('@"MNGGameDataFolder." TOTK_GAME_DIR', source)   # the folder chosen when the app asked (game_data_ask)
        self.assertIn('ROMFS_IMAGE_SIZE', source)
        self.assertIn("info['MNGGameDataFolder']", (Path(build_app.__file__)).read_text())


if __name__ == '__main__':
    unittest.main()
