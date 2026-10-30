"""--cheats: only the cheat file for the selected version's build ID is built into the app."""
from pathlib import Path
import tempfile
import unittest

import build_app

BID = '082CE09B06E33A12'
TEXT = '[Title]\n\n[Arrows]\n580F0000 04646C60\n640F1000 00000000 000003E6\n'


class CheatSelectionTests(unittest.TestCase):
    def test_build_id_from_nso_header(self):
        with tempfile.TemporaryDirectory() as folder:
            tree = Path(folder)
            main = tree/'extracted/v0/exefs/main'
            main.parent.mkdir(parents=True)
            main.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(BID) + bytes(0x40))
            self.assertEqual(build_app.main_build_id(tree, 'v0'), BID)

    def test_breath_of_the_wild_build_id(self):   # the same lookup for a Breath of the Wild version folder
        with tempfile.TemporaryDirectory() as folder:
            tree = Path(folder)
            main = tree/'extracted/botw160/exefs/main'
            main.parent.mkdir(parents=True)
            main.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex('8E9978D50BDD20B4') + bytes(0x40))
            self.assertEqual(build_app.main_build_id(tree, 'botw160'), '8E9978D50BDD20B4')
            (tree/'8E9978D50BDD20B4.txt').write_text(TEXT)
            path, count = build_app.select_cheats(tree, '8E9978D50BDD20B4')
            self.assertEqual((path.name, count), ('8E9978D50BDD20B4.txt', 2))

    def test_folder_picks_this_build_only(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'168DD518D925C7A3.txt').write_text(TEXT)
            (root/f'{BID.lower()}.txt').write_text(TEXT)
            path, count = build_app.select_cheats(root, BID)
            self.assertEqual(path.name.upper(), f'{BID}.TXT')
            self.assertEqual(count, 2)

    def test_other_version_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            other = root/'168DD518D925C7A3.txt'
            other.write_text(TEXT)
            with self.assertRaisesRegex(build_app.BuildError, BID):
                build_app.select_cheats(other, BID)
            with self.assertRaisesRegex(build_app.BuildError, 'no 082CE09B06E33A12.txt'):
                build_app.select_cheats(root, BID)

    def test_file_without_sections_is_refused(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/f'{BID}.txt'
            path.write_text('580F0000 04646C60\n')
            with self.assertRaisesRegex(build_app.BuildError, 'no \\[cheat name\\] sections'):
                build_app.select_cheats(path, BID)


if __name__ == '__main__':
    unittest.main()
