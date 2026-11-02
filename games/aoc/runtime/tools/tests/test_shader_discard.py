"""Vertex storage writes survive rasterizer discard and are reflected as writes."""
import ast
from pathlib import Path
import re
import sys
import types
import unittest


SOURCE = Path(__file__).resolve().parents[2] / 'gfx/tools/dmsh_build.py'
tree = ast.parse(SOURCE.read_text())
names = {'vs_tf', 'vs_discard', 'reflect'}
scope = {'re': re, 'sys': sys, 'TF_SLOT': 8,
         'SSBO': next(ast.literal_eval(n.value) for n in tree.body
                      if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'SSBO' for t in n.targets)),
         'dmsh_geometry': types.SimpleNamespace(HELPERS={})}
exec(compile(ast.Module(body=[n for n in tree.body
                             if isinstance(n, ast.FunctionDef) and n.name in names],
                        type_ignores=[]), str(SOURCE), 'exec'), scope)


class Discard(unittest.TestCase):
    def test_geometry_storage_uses_its_own_stage_table(self):
        msl = '''struct spvDescriptorSetBuffer0 {
    device Words* gp_s0 [[id(4)]];
};'''
        for offset, expected in [(0x410, 0), (0x500, 15), (0x510, None), (0x414, None), (0x110, None)]:
            with self.subTest(offset=offset):
                resources = {'sbufs': [{'binding': 40, 'cb_slot': 0,
                                       'cb_offset': offset // 4, 'flags': 0}]}
                binding, = scope['reflect']('test', 'gs', msl, resources)
                self.assertEqual(binding['reg'], expected)
                self.assertFalse(binding['rw'])

    def test_void_entry_keeps_stores_before_early_and_final_returns(self):
        source = '''struct main0_out { float4 gl_Position [[position]]; };
vertex main0_out main0(uint vertex [[vertex_id]], device uint* result [[buffer(0)]])
{
    main0_out out = {};
    if (vertex == 0) { result[0] = 17; return out; }
    result[vertex] = vertex * 3;
    return out;
}
'''
        entry = scope['vs_discard'](source)
        self.assertIn('vertex void main0_discard(', entry)
        self.assertIn('result[0] = 17;', entry)
        self.assertIn('result[vertex] = vertex * 3;', entry)
        self.assertEqual(entry.count('return;'), 2)
        self.assertNotIn('return out;', entry)
        self.assertNotIn('dm_tf', entry)
        self.assertNotIn('dm_vid', entry)  # Reuse the original vertex ID.

    def test_storage_write_flag_is_bit_zero(self):
        msl = '''struct spvDescriptorSetBuffer0 {
    device Words* vp_s0 [[id(0)]];
};'''
        for flags, expected in [(0, False), (1, True), (2, False), (3, True)]:
            with self.subTest(flags=flags):
                resources = {'sbufs': [{'binding': 0, 'cb_slot': 0,
                                       'cb_offset': 0x110 // 4, 'flags': flags}]}
                binding, = scope['reflect']('test', 'vs', msl, resources)
                self.assertEqual(binding['reg'], 0)
                self.assertEqual(binding['rw'], expected)


if __name__ == '__main__':
    unittest.main()
