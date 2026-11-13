"""games/totk/scripts/import_dump.py walk(): a dump folder's RomFS is packed from its game files only, never from what a
dumper or macOS leaves beside them (2026-11-12: 14 .DS_Store files changed Bananza's RomFS image's SHA-256)."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

GAMES = Path(__file__).resolve().parents[1] / 'games'


def load(game):
    spec = importlib.util.spec_from_file_location(f'import_dump_{game}', GAMES / game / 'scripts/import_dump.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


COPIES = {game: load(game) for game in ('totk', 'oot', 'mkw')}   # each game's own copy


class WalkTests(unittest.TestCase):
    def test_only_game_files(self):
        for game, dump in COPIES.items():
            with self.subTest(game=game), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'Pack').mkdir()
                (root / 'Pack/a.pack').write_bytes(b'x' * 3)
                (root / 'b.bin').write_bytes(b'y')
                for junk in ('.DS_Store', '.listing', 'Pack/.DS_Store', 'Pack/.listing', 'Pack/._a.pack'):
                    (root / junk).write_bytes(b'z')
                self.assertEqual(dump.walk(root), [('Pack/a.pack', 3), ('b.bin', 1)])

    def test_the_names_left_out(self):
        for game, dump in COPIES.items():
            for name in ('.DS_Store', '.listing', '._a.pack'):
                self.assertTrue(dump.not_game_file(name), (game, name))
            for name in ('a.pack', '.hidden_game_file', 'DS_Store'):
                self.assertFalse(dump.not_game_file(name), (game, name))


if __name__ == '__main__':
    unittest.main()
