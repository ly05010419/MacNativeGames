"""shader_list.py without game data: list rows, and adding the bind states a played cache met."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('eow_shader_list', HERE / 'scripts/shader_list.py')
LIST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(LIST)

K1, K2 = '1' * 64, '2' * 64
DEFAULT = '-b7530f698e76'


class RowTests(unittest.TestCase):
    def test_rows_are_checked(self):
        for bad in (['V', 'notakey', '{}'], ['V', K1, '{"pso":{}}'], ['V', K1, 'not json'], ['V', K1, '[]'],
                    ['S', K1 + DEFAULT + '_b', '1'], ['V', K1]):
            self.assertIsNone(LIST.parse_row(bad), bad)
        self.assertEqual(LIST.parse_row(['V', K1, '{"tex":{"0":"cube"}}']), (K1, dict(tex={'0': 'cube'})))

    def test_read_names_a_bad_line(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'list.tsv'
            path.write_text(LIST.HEADER + f'V\t{K1}\t{{}}\nR\tabc\n')
            with self.assertRaisesRegex(LIST.ListError, ':4: expected V'):
                LIST.read(path)

    def test_write_sorts_and_reads_back(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'list.tsv'
            rows = [(K2, dict(tex={'0': 'cube'})), (K1, dict(fout={'0': 'uint'}))]
            LIST.write(path, rows)
            self.assertTrue(path.read_text().startswith(LIST.HEADER))
            self.assertEqual(LIST.read(path), sorted(rows))


class AddTests(unittest.TestCase):
    def test_add_takes_only_unserved_states_of_seed_shaders(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            seed, cache, path = root / 'seed', root / 'cache', root / 'list.tsv'
            seed.mkdir()
            cache.mkdir()
            (seed / f'{K1}.variants').write_text(json.dumps([dict(variant=K1 + DEFAULT, deps=dict(tex={'0': '2d'}))]))
            (cache / f'{K1}.variants').write_text(json.dumps([dict(variant=K1 + '-a', deps=dict(tex={'0': 'cube'})),
                                                              dict(variant=K1 + '-b', deps=dict(tex={'0': '2d'}))]))
            (cache / f'{K2}.variants').write_text(json.dumps([dict(variant=K2 + '-c', deps=dict(tex={'0': '3d'}))]))
            self.assertEqual(LIST.add(path, seed, [cache]), 1)
            self.assertEqual(LIST.read(path), [(K1, dict(tex={'0': 'cube'}))])
            self.assertEqual(LIST.add(path, seed, [cache]), 0)   # again: nothing new

    def test_the_shipped_list_reads(self):
        rows = LIST.read(HERE / 'assets/shader_list.tsv')
        self.assertEqual(len(rows), len({(k, LIST.canonical(d)) for k, d in rows}))
        self.assertTrue((HERE / 'assets/shader_list.tsv').read_text().startswith(LIST.HEADER))


if __name__ == '__main__':
    unittest.main()
