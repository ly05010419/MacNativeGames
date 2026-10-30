"""The translator consumes public shader keys without an original-resource column."""
import ast
from pathlib import Path
import re
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[2] / 'gfx/tools/dmsh_build.py'
tree = ast.parse(SOURCE.read_text())
scope = {'re': re}
exec(compile(ast.Module(body=[node for node in tree.body
                             if isinstance(node, ast.FunctionDef) and node.name == 'read_shader_map'],
                        type_ignores=[]), str(SOURCE), 'exec'), scope)


class ShaderMap(unittest.TestCase):
    def test_all_public_keys_are_read(self):
        path = SOURCE.parents[2] / 'games/0100965017338000/shader_map.tsv'
        rows, locations = scope['read_shader_map'](path)
        self.assertEqual(len(locations), 25693)
        self.assertEqual(rows.keys(), locations.keys())
        key = '0123456789abcdef'
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'map.tsv'
            path.write_text('# public identifiers\npackage\tindex\tstage\tdm_ctl_sha16\n'
                            f'vs_test.bin\t7\tvertex\t{key}\n')
            rows, locations = scope['read_shader_map'](path)
            self.assertEqual(locations, {key: ('vs_test.bin', 7)})
            self.assertEqual(len(rows[key].rstrip().split(b'\t')), 4)

    def test_rejects_old_path_column_and_empty_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'map.tsv'
            path.write_text('vs_test.bin\t0\tvertex\t0123456789abcdef\t/private/resource\n')
            with self.assertRaisesRegex(ValueError, 'package/index/stage/key only'):
                scope['read_shader_map'](path)
            path.write_text('package\tindex\tstage\tdm_ctl_sha16\n')
            with self.assertRaisesRegex(ValueError, 'no shader keys'):
                scope['read_shader_map'](path)
