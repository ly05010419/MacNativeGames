"""Mesh pipeline rows (M) of the shared shader list: "M <task variant or -> <pipeline key>", the mesh shader in the key's
vertex place. A row must survive a save, name all three of its shaders, and be renamed as a whole."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import shader_list as sl  # noqa: E402

DEFAULT = sl.DEFAULT_SUFFIX
TASK = 'a' * 64 + f'-{DEFAULT}_b'
MESH = 'b' * 64 + f'-{DEFAULT}_b'
FRAG = 'c' * 64 + f'-{DEFAULT}_b'
OTHER = 'd' * 64 + f'-{DEFAULT}_b'


def key(ms=MESH, fs=FRAG):
    b = bytearray(sl.SKEY_SIZE)
    b[:sl.NAME] = ms.encode().ljust(sl.NAME, b'\0')
    b[sl.NAME:2 * sl.NAME] = fs.encode().ljust(sl.NAME, b'\0')
    b[160], b[161] = 4, 1   # mesh and fragment stages
    return b.hex()


class RowTests(unittest.TestCase):
    def test_a_row_with_and_without_a_task_shader(self):
        self.assertEqual(sl.warm_row(['M', TASK, key()]), ('M', TASK, key()))
        self.assertEqual(sl.warm_row(['M', '-', key()]), ('M', '-', key()))

    def test_malformed_rows_are_refused(self):
        self.assertIsNone(sl.warm_row(['M', 'not-a-variant', key()]))
        self.assertIsNone(sl.warm_row(['M', TASK, 'xyz']))
        self.assertIsNone(sl.warm_row(['M', TASK]))

    def test_a_row_names_all_its_shaders(self):
        self.assertEqual(sorted(sl.row_variants(('M', TASK, key()))), sorted([TASK, MESH, FRAG]))
        self.assertEqual(sorted(sl.row_variants(('M', '-', key()))), sorted([MESH, FRAG]))

    def test_renaming_covers_the_task_and_the_key(self):
        row = sl.rename_warm(('M', TASK, key()), {TASK: OTHER, FRAG: OTHER})
        self.assertEqual(row[1], OTHER)
        self.assertEqual(sorted(sl.row_variants(row)), sorted([OTHER, MESH, OTHER]))
        self.assertEqual(sl.rename_warm(('M', '-', key()), {TASK: OTHER})[1], '-')


class ListTests(unittest.TestCase):
    def test_rows_survive_merge_save_and_load(self):
        with tempfile.TemporaryDirectory() as d:
            assets = Path(d) / 'games/oot/assets'
            assets.mkdir(parents=True)
            source, target = Path(d) / 'pso_user.tsv', assets / 'shader_list.tsv'
            source.write_text(f'M\t{TASK}\t{key()}\nM\t-\t{key(fs="")}\n')
            lst = sl.ShaderList.load(target)
            lst.add_rows(source, 'm')
            self.assertEqual(lst.refused, 0)
            lst.save(target)
            self.assertNotIn(key(), target.read_text())   # kept packed, as R rows are
            again = sl.ShaderList.load(target)
            self.assertEqual(list(again.warm), [('M', TASK, key()), ('M', '-', key(fs=''))])
            self.assertIn('2 M', again.counts())
            self.assertEqual(again.export('m').splitlines()[1:], [f'M\t{TASK}\t{key()}', f'M\t-\t{key(fs="")}'])

    def test_a_row_of_an_unknown_bind_state_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            source = Path(d) / 'pso_user.tsv'
            source.write_text(f'M\t{"a" * 64}-0123456789ab_b\t{key()}\n')
            lst = sl.ShaderList()
            lst.add_rows(source, 'm')
            self.assertEqual(lst.refused, 1)
            self.assertEqual(lst.warm, {})


if __name__ == '__main__':
    unittest.main()
