"""Decode interleaved feedback declarations and compile their generated entries."""
import ast
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
nodes = [n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef) and n.name in ('tf_layout', 'vs_tf')]
scope = dict(re=re, struct=struct, sys=sys, TF_SLOT=8)
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), scope)

SOURCE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out { float4 a [[user(locn0)]]; float4 b [[user(locn1)]]; float4 p [[position]]; };
vertex main0_out main0(uint n [[vertex_id]]) {
    main0_out out = {};
    out.a = float4(n, n + 10, n + 20, n + 30);
    out.b = float4(n + 40, n + 50, n + 60, n + 70);
    out.p = float4(0, 0, 0, 1);
    return out;
}
'''
LAYOUT = [{'stride': 20, 'outputs': [[0, 2], [1, 3]]}, [], [], []]
SPLIT_LAYOUT = [{'stride': 12, 'outputs': [[0, 3]]}, {'stride': 4, 'outputs': [[1, 1]]}, {'stride': 4, 'outputs': [[1, 1]]}, []]


def fixture(geometry=False):
    return SOURCE + scope['vs_tf'](SOURCE, LAYOUT, geometry)


class FeedbackTests(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('DM_TEST_TRANSFORM_FEEDBACK'), 'set DM_TEST_TRANSFORM_FEEDBACK to the native t_transform_feedback executable')
    def test_native_gpu_replay(self):
        with tempfile.TemporaryDirectory(prefix='feedback_') as directory:
            source = Path(directory) / 'feedback.metal'
            source.write_text(fixture())
            Path(str(source) + '.geo').write_text(fixture(True))
            Path(str(source) + '.split').write_text(SOURCE + scope['vs_tf'](SOURCE, SPLIT_LAYOUT))
            result = subprocess.run([os.environ['DM_TEST_TRANSFORM_FEEDBACK'], str(source)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
    def test_component_counts_are_bytes_and_stream_order_is_preserved(self):
        control = bytearray(0x880)
        struct.pack_into('<H', control, 0x6dc, 20)
        control[0x6e8] = 2
        control[0xdc:0xde] = bytes([32, 33])
        control[0x4dc:0x4de] = bytes([2, 3])
        self.assertEqual(scope['tf_layout']('test', control), LAYOUT)

    def test_interleaved_stores_keep_bit_patterns_and_stride(self):
        source = fixture()
        for offset, value in enumerate(('a.x', 'a.y', 'b.x', 'b.y', 'b.z')):
            self.assertIn(f'dm_linear * 5 + {offset}] = as_type<uint>(out.{value})', source)
        self.assertIn('dm_tf.offset[0] / 4', source)
        self.assertIn('dm_linear < dm_tf.args.x', source)

    def test_three_separate_single_output_streams(self):
        control = bytearray(0x880)
        struct.pack_into('<4H', control, 0x6dc, 12, 4, 4, 0)
        for b, (location, components) in enumerate(((32, 3), (33, 1), (33, 1))):
            control[0x6e8 + b] = 1
            control[0xdc + b * 128] = location
            control[0x4dc + b * 128] = components
        self.assertEqual(scope['tf_layout']('test', control), SPLIT_LAYOUT)

    def test_geometry_gathers_after_calculating_linear_capture_index(self):
        source = fixture(True)
        self.assertLess(source.index('uint dm_linear'), source.index('n = dm_tf.order[dm_linear]'))

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_vertex_and_geometry_entries_compile_for_both_platforms(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for geometry in (False, True):
                source = root / ('geometry.metal' if geometry else 'vertex.metal')
                source.write_text(fixture(geometry))
                for sdk in ('macosx', 'iphoneos'):
                    with self.subTest(geometry=geometry, sdk=sdk):
                        result = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / sdk), str(source), '-o', str(root / (sdk + '.air'))], capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture':
        Path(sys.argv[2]).write_text(fixture())
        Path(sys.argv[2] + '.geo').write_text(fixture(True))
        Path(sys.argv[2] + '.split').write_text(SOURCE + scope['vs_tf'](SOURCE, SPLIT_LAYOUT))
    else:
        unittest.main()
