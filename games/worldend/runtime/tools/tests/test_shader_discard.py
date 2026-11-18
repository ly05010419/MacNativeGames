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
         'SSBO': {'vs': 0x110, 'cs': 0x310, 'ps': 0x510},
         'dmsh_geometry': types.SimpleNamespace(HELPERS={})}
exec(compile(ast.Module(body=[n for n in tree.body
                             if isinstance(n, ast.FunctionDef) and n.name in names],
                        type_ignores=[]), str(SOURCE), 'exec'), scope)


class Discard(unittest.TestCase):
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
