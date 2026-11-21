"""Shadow vertex inputs (dm/gfx/tools/dmsh_vin.py): small vertex shaders in the translator's MSL shape, no game needed. The variant must give each integer input a float4 shadow and
then other-sign shadows while attribute indices 16-30 last, OR them into the input in both entry points, leave the transform feedback entry out, and compile with the Metal compiler
for macOS and iOS (temporary files only in a temporary folder)."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_vin

def shader(types, tf=False):
    """types: the MSL type of each input in location order ('float4' / 'uint4' / 'int4')"""
    ins = ''.join(f'    {t} m_{l} [[attribute({l})]];\n' for l, t in enumerate(types))
    use = ''.join(f'    acc += float4(as_type<float4>(in.m_{l}));\n' if t != 'float4' else f'    acc += in.m_{l};\n' for l, t in enumerate(types))
    main = lambda name: f'\nvertex main0_out {name}(main0_in in [[stage_in]], constant float4& u [[buffer(0)]])\n{{\n    main0_out out = {{}};\n    float4 acc = u;\n{use}    out.gl_Position = acc;\n    return out;\n}}\n'
    src = '#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 gl_Position [[position]];\n};\n\nstruct main0_in\n{\n' + ins.rstrip('\n') + '\n};\n'
    src += main('main0') + main('main0_z')
    if tf: src += '\nvertex void main0_tf(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]], device uint* dm_tf0 [[buffer(8)]])\n{ dm_tf0[dm_vid] = 0u; }\n'
    return src

class ShadowInputs(unittest.TestCase):
    def compiles(self, msl):
        with tempfile.TemporaryDirectory(prefix='dmsh_vin_test_') as d:
            p = Path(d) / 'v.metal'; p.write_text(msl)
            for sdk in ('macosx', 'iphoneos'):
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', str(p), '-o', str(Path(d) / f'v.{sdk}.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{sdk}: {r.stderr[:400]}')

    def test_one_uint_input(self):
        v, sh = dmsh_vin.variant(shader(['float4', 'uint4']))
        self.assertEqual(sh, [{'location': 1, 'float': 16, 'sint': 17}])
        self.assertIn('    int4 m_1_vo [[attribute(17)]];', v)
        self.assertEqual(v.count('in.m_1 = dm_in.m_1 | as_type<uint4>(dm_in.m_1_vf) | as_type<uint4>(dm_in.m_1_vo);'), 2)   # main0 and main0_z
        self.assertEqual(v.count('    in.m_0 = dm_in.m_0;'), 2)
        self.compiles(v)

    def test_int_input_gets_a_uint_shadow(self):
        v, sh = dmsh_vin.variant(shader(['int4']))
        self.assertEqual(sh, [{'location': 0, 'float': 16, 'uint': 17}])
        self.assertIn('in.m_0 = dm_in.m_0 | as_type<int4>(dm_in.m_0_vf) | as_type<int4>(dm_in.m_0_vo);', v)
        self.compiles(v)

    def test_eight_inputs_fill_every_index(self):
        v, sh = dmsh_vin.variant(shader(['uint4'] * 8))
        self.assertEqual([s['float'] for s in sh], list(range(16, 24)))
        self.assertEqual([s.get('sint') for s in sh], list(range(24, 31)) + [None])   # 15 indices: the last input has only its float shadow
        self.assertIn('in.m_7 = dm_in.m_7 | as_type<uint4>(dm_in.m_7_vf);', v)
        self.compiles(v)

    def test_more_inputs_than_indices(self):
        v, sh = dmsh_vin.variant(shader(['uint4'] * 16))
        self.assertEqual(len(sh), 15)   # the 16th integer input gets no shadow: its formats keep the CPU conversion
        self.assertIn('    in.m_15 = dm_in.m_15;', v)
        self.compiles(v)

    def test_transform_feedback_entry_left_out(self):
        v, sh = dmsh_vin.variant(shader(['uint4'], tf=True))
        self.assertNotIn('main0_tf', v)
        self.compiles(v)

    def test_split_input_array(self):
        src = shader(['float4', 'float4', 'uint4']).replace('m_0', 'm_7_0').replace('m_1', 'm_7_1').replace('m_2', 'm_7_2')   # SPIRV-Cross splits an input array per element
        self.assertEqual(dmsh_vin.variant(shader(['float4', 'float4']).replace('m_0', 'm_7_0').replace('m_1', 'm_7_1'))[1], 'no integer input')
        v, sh = dmsh_vin.variant(src)
        self.assertEqual(sh, [{'location': 2, 'float': 16, 'sint': 17}])
        self.assertIn('in.m_7_2 = dm_in.m_7_2 | as_type<uint4>(dm_in.m_7_2_vf) | as_type<uint4>(dm_in.m_7_2_vo);', v)
        self.compiles(v)

    def test_refusals(self):
        self.assertEqual(dmsh_vin.variant(shader(['float4']))[1], 'no integer input')
        no_input = re.sub(r'\nstruct main0_in\n\{\n.*?\n\};\n', '\n', shader(['uint4']), flags=re.S).replace('main0_in in [[stage_in]], ', '')
        self.assertEqual(dmsh_vin.variant(no_input), (None, 'no integer input'))   # no stage input at all: nothing to shadow, not a refusal
        self.assertIsNone(dmsh_vin.variant(shader(['uint4']).replace('[[attribute(0)]]', '[[attribute(16)]]'))[0])
        self.assertIsNone(dmsh_vin.variant(shader(['uint4']).replace('uint4 m_0', 'uint2 m_0'))[0])

if __name__ == '__main__':
    unittest.main()
