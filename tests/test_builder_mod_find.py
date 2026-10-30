"""tools/builder/mod_find.py (S5.4 C1, C4; S5.5 C3, C4): the layouts mods come in, packs of several, TKMM packages.
Real folders in a temporary folder; nothing else is touched. Archives: test_builder_mod_archive.py."""
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch

from tools.builder import mod_find
from tools.builder.mod_find import Refused

TOTK, BOTW = '0100F2C0115B6000', '01007EF00011E000'
TOPS = {'Pack', 'UI', 'Font'}


class Base(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        self.tmp = Path(tmp).resolve()

    def tree(self, *files):
        for name in files:
            (self.tmp / name).parent.mkdir(parents=True, exist_ok=True)
            (self.tmp / name).write_bytes(b'x')
        return self.tmp

    def find(self, path):
        """The one mod found (S5.5: find returns every candidate)."""
        [found] = mod_find.find(self.tmp / path, TOPS, TOTK)
        return found

    def refused(self, path):
        with self.assertRaises(Refused) as caught:
            self.find(path)
        return caught.exception.reason


class Layouts(Base):
    def test_the_pair_at_the_top(self):
        self.tree('Mod/romfs/Pack/A.pack.zs', 'Mod/exefs/main.pchtxt')
        found = self.find('Mod')
        self.assertEqual((found.romfs, found.exefs, found.notes),
                         (self.tmp / 'Mod/romfs', self.tmp / 'Mod/exefs', ()))

    def test_the_romfs_folder_itself_picked(self):
        self.tree('Mod/romfs/Pack/A.pack.zs')
        self.assertEqual(self.find('Mod/romfs').romfs, self.tmp / 'Mod/romfs')

    def test_one_folder_too_many_and_the_emulator_trees(self):
        for layout in ('A/A', f'B/contents/{TOTK}', f'C/atmosphere/contents/{TOTK}', 'D/1/2/3'):
            self.tree(f'{layout}/romfs/UI/x.bfres')
            self.assertEqual(self.find(layout.split('/')[0]).romfs, self.tmp / layout / 'romfs', layout)

    def test_deeper_than_four_levels_is_not_searched(self):
        self.tree('E/1/2/3/4/romfs/UI/x.bfres')
        self.assertEqual(self.refused('E'), 'nothing')

    def test_a_flat_romfs_and_one_unknown_folder(self):
        self.tree('Flat/Pack/A.pack.zs', 'Flat/UI/x.bfres', 'Flat/readme.txt')
        self.assertEqual((self.find('Flat').romfs, self.find('Flat').exefs), (self.tmp / 'Flat', None))
        self.tree('Flat/Screenshots/a.png')
        self.assertEqual(self.refused('Flat'), 'nothing')

    def test_another_game(self):
        self.tree(f'{BOTW}/romfs/Pack/x')
        self.assertEqual(self.refused(BOTW), 'other_game')
        self.tree(f'Pack2/contents/{BOTW}/romfs/Pack/x')
        self.assertEqual(self.refused('Pack2'), 'other_game')

    def test_a_pack_for_both_games_adds_this_one(self):
        self.tree(f'Both/contents/{BOTW}/romfs/Pack/x', f'Both/contents/{TOTK}/romfs/Pack/y')
        self.assertEqual(self.find('Both').romfs, self.tmp / f'Both/contents/{TOTK}/romfs')

    def test_a_pack_gives_each_candidate_and_too_many_is_several(self):           # S5.5 C3
        self.tree('Many/One/romfs/Pack/x', 'Many/Two/exefs/main.pchtxt', 'Many/Two/romfs/UI/y')
        found = mod_find.find(self.tmp / 'Many', TOPS, TOTK)
        self.assertEqual([(f.name, f.romfs, bool(f.exefs)) for f in found],
                         [('One', self.tmp / 'Many/One/romfs', False), ('Two', self.tmp / 'Many/Two/romfs', True)])
        self.tree(*(f'Lots/{n:02}/romfs/Pack/x' for n in range(mod_find.MAX_PACK + 1)))
        with self.assertRaises(Refused) as caught:
            mod_find.find(self.tmp / 'Lots', TOPS, TOTK)
        self.assertEqual((caught.exception.reason, caught.exception.detail), ('several', '00, 01, 02, 03'))

    def test_a_tkmm_package_is_named(self):                                         # S5.5 C4
        self.tree('Sword/The sword.tkcl', 'Sword/readme.txt')
        self.assertEqual(self.refused('Sword'), 'tkmm_package')

    def test_cheats_only_and_cheats_beside(self):
        self.tree('Cheat/cheats/ABCDEF.txt')
        self.assertEqual(self.refused('Cheat'), 'cheats_only')
        self.tree('Both/romfs/Pack/x', 'Both/cheats/ABCDEF.txt')
        self.assertEqual(self.find('Both').notes, ('cheats_ignored',))

    def test_nothing_and_too_big(self):
        (self.tmp / 'Empty').mkdir()
        self.assertEqual(self.refused('Empty'), 'nothing')
        self.tree('Big/a/x', 'Big/b/x', 'Big/c/x')
        with patch.object(mod_find, 'MAX_WALK', 2):
            self.assertEqual(self.refused('Big'), 'too_big')

    def test_symbolic_links_are_not_followed_and_mac_forks_are_skipped(self):
        self.tree('Out/romfs/Pack/x')
        (self.tmp / 'L').mkdir()
        (self.tmp / 'L/link').symlink_to(self.tmp / 'Out')
        self.assertEqual(self.refused('L'), 'nothing')
        self.tree('Z/Mod/romfs/Pack/x', 'Z/__MACOSX/Mod/romfs/Pack/._x')
        self.assertEqual(self.find('Z').romfs, self.tmp / 'Z/Mod/romfs')


if __name__ == '__main__':
    unittest.main()
