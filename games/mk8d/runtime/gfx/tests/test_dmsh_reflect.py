"""Production reflection conversion with synthetic translator descriptors."""
import ast
from pathlib import Path
import re
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_geometry


def builder_reflection():
    # Isolate pure reflection code from the builder's CLI/profile initialization.
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    names = {'reflect', 'decode_tex', 'decode_ubo_tex', 'const_sampler', 'c2_array'}
    selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in names)
                or (isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SSBO' for t in n.targets))]
    ns = {'re': re, 'dmsh_geometry': dmsh_geometry}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    return ns['reflect']


REFLECT = builder_reflection()


def bindings(stage, offsets, flags, cb_slot=0):
    source = 'struct spvDescriptorSetBuffer0\n{\n' + ''.join(
        f'    device uint* gp_s{i} [[id({i})]];\n' for i in range(len(offsets))) + '};\n'
    res = {'sbufs': [{'binding': 40 + i, 'slot': i, 'cb_slot': cb_slot,
                      'cb_offset': off // 4, 'flags': flag}
                     for i, (off, flag) in enumerate(zip(offsets, flags))]}
    return REFLECT('synthetic', stage, source, res)


class StorageBuffers(unittest.TestCase):
    def test_geometry_driver_table_offsets(self):
        got = bindings('gs', [0x410, 0x430], [1, 1])
        self.assertEqual([(b['kind'], b['reg']) for b in got], [('sbuf', 0), ('sbuf', 2)])

    def test_write_flag_from_translator_json(self):
        # BufferUsageFlags.Write = 1 << 0; Program.cs serializes it unchanged.
        got = bindings('cs', [0x310, 0x320], [1, 0])
        self.assertEqual([b['rw'] for b in got], [True, False])

    def test_write_flag_for_address_in_guest_uniform(self):
        got = bindings('gs', [0x38], [1], cb_slot=3)
        self.assertEqual((got[0]['kind'], got[0]['reg'], got[0]['off'], got[0]['rw']),
                         ('sbuf_ubo', 0, 0x38, True))

    def test_existing_stage_tables(self):
        for stage, base in [('vs', 0x110), ('cs', 0x310), ('ps', 0x510)]:
            with self.subTest(stage=stage):
                got = bindings(stage, [base, base + 15 * 16], [0, 0])
                self.assertEqual([b['reg'] for b in got], [0, 15])

    def test_unknown_or_unaligned_offsets_stay_unresolved(self):
        self.assertIsNone(bindings('gs', [0x414], [1])[0]['reg'])
        self.assertIsNone(bindings('hs', [0x410], [1])[0]['reg'])


if __name__ == '__main__':
    unittest.main()
