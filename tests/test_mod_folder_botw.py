"""mod_folder.py: each game version's RomFS mods go into that game's own folder, the one the shared app launcher
points the game at (app/launcher.m: <data> is MacNativeGames/<GAME_FOLDER>, TOTK_ROMFS_MODS = <data>/mods/<version>).
Breath of the Wild's versions used to resolve to the TOTK folder, where its app never looks."""
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import mod_folder      # noqa: E402


class ModsFolder(unittest.TestCase):
    def test_each_game_has_its_own_folder(self):
        with tempfile.TemporaryDirectory() as home, patch.dict(os.environ, HOME=home):
            data = Path(home)/'Library/Application Support/MacNativeGames'
            for version, game in (('botw160', 'BotW'), ('botw190', 'BotW'), ('v0', 'TOTK'), ('v121', 'TOTK'),
                                  ('v143', 'TOTK')):
                with self.subTest(version=version):
                    self.assertEqual(mod_folder.mods_folder(version), data/game/'mods'/version)

    def test_the_launcher_names_the_same_folders(self):
        launcher = (ROOT/'games/totk/app/launcher.m').read_text()
        self.assertIn('@"Library/Application Support/MacNativeGames/" GAME_FOLDER', launcher)
        versions = (ROOT/'games/totk/runtime/game_version.h').read_text()
        for folder in ('"TOTK"', '"BotW"'):
            self.assertIn(folder, versions)


if __name__ == '__main__':
    unittest.main()
