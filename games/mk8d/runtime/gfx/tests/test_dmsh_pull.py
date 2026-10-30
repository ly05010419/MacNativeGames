"""Pulling vertex inputs (dm/gfx/tools/dmsh_pull.py): small vertex shaders in the translator's MSL shape, no game needed. The variant must drop [[stage_in]] from main0 and main0_z,
add the layout block at buffer 16 and the vertex / instance / base instance ids it lacks (keeping the ones the shader declares), fill every input through dm_fetch with as_type to
its declared type, leave the other entry points out, decline shaders it does not fit, and compile with the Metal compiler for macOS and iOS (temporary files only in a temporary folder)."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_pull

def shader(types, extra='', ids='', tf=False, gs=False):
    """types: the MSL type of each input in location order ('float4' / 'uint4' / 'int4'); ids: extra parameters of main0 / main0_z (e.g. a [[vertex_id]])"""
    ins = ''.join(f'    {t} m_{l} [[attribute({l})]];\n' for l, t in enumerate(types))
    use = ''.join(f'    acc += float4(as_type<float4>(in.m_{l}));\n' if t != 'float4' else f'    acc += in.m_{l};\n' for l, t in enumerate(types))
    main = lambda name: (f'\nvertex main0_out {name}(main0_in in [[stage_in]], constant float4& u [[buffer(0)]]{ids})\n{{\n    main0_out out = {{}};\n    float4 acc = u;\n{use}'
                         f'    out.gl_Position = acc;\n    return out;\n}}\n')
    src = ('#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\nstruct main0_out\n{\n    float4 gl_Position [[position, invariant]];\n};\n\n'
           'struct main0_in\n{\n' + ins.rstrip('\n') + '\n};\n' + extra)
    src += main('main0') + main('main0_z')
    if tf: src += '\nvertex void main0_tf(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]], device uint* dm_tf0 [[buffer(8)]])\n{\n    dm_tf0[dm_vid] = 0u;\n}\n'
    if gs: src += '\nvertex void main0_gs(main0_in in [[stage_in]], constant float4& u [[buffer(0)]], uint dm_vid [[vertex_id]])\n{\n    (void)in;\n}\n'
    return src

class PullingInputs(unittest.TestCase):
    def compiles(self, msl):
        with tempfile.TemporaryDirectory(prefix='dmsh_pull_test_') as d:
            p = Path(d) / 'v.metal'; p.write_text(msl)
            for sdk in ('macosx', 'iphoneos'):
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fpreserve-invariance', str(p), '-o', str(Path(d) / f'v.{sdk}.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, f'{sdk}: {r.stderr[:600]}')

    def test_inputs_through_the_layout_block(self):
        v, info = dmsh_pull.variant(shader(['float4', 'uint4', 'int4']))
        self.assertEqual(info, {'entry': 'main0', 'entry_z': 'main0_z', 'slot': 16, 'inputs': [0, 1, 2]})
        self.assertNotIn('[[stage_in]]', v)
        self.assertNotIn('[[attribute(', v)
        self.assertEqual(v.count('constant dm_vlayout& dm_lay [[buffer(16)]]'), 2)   # main0 and main0_z
        self.assertEqual(v.count('    in.m_1 = as_type<uint4>(dm_fetch(dm_lay, 1u, dm_vid, dm_iid, dm_bi));'), 2)
        self.assertEqual(v.count('    in.m_2 = as_type<int4>(dm_fetch(dm_lay, 2u, dm_vid, dm_iid, dm_bi));'), 2)
        self.assertEqual(v.count('uint dm_vid [[vertex_id]], uint dm_iid [[instance_id]], uint dm_bi [[base_instance]]'), 2)
        self.compiles(v)

    def test_keeps_the_ids_the_shader_declares(self):
        v, info = dmsh_pull.variant(shader(['float4'], ids=', uint gl_InstanceIndex [[instance_id]], uint gl_BaseInstance [[base_instance]], uint gl_VertexIndex [[vertex_id]]'))
        self.assertEqual(v.count('dm_fetch(dm_lay, 0u, gl_VertexIndex, gl_InstanceIndex, gl_BaseInstance)'), 2)
        self.assertNotIn('uint dm_vid [[vertex_id]]', v); self.assertNotIn('uint dm_iid [[instance_id]]', v); self.assertNotIn('uint dm_bi [[base_instance]]', v)   # no second id
        self.compiles(v)

    def test_other_entry_points_left_out(self):
        v, info = dmsh_pull.variant(shader(['float4', 'float4'], tf=True, gs=True))
        self.assertNotIn('main0_tf', v); self.assertNotIn('main0_gs', v)
        self.compiles(v)

    def test_declined(self):
        no_input = shader(['float4']).replace('struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n};\n', '').replace('main0_in in [[stage_in]], ', '')
        self.assertEqual(dmsh_pull.variant(no_input), (None, 'no stage input'))
        self.assertEqual(dmsh_pull.variant(shader(['float4'], extra='constant float4& x [[buffer(16)]];\n'))[1], 'buffer 16 is taken')
        self.assertEqual(dmsh_pull.variant(shader(['float4']).replace('[[attribute(0)]]', '[[attribute(17)]]'))[1], 'an input at attribute 16 or above')

if __name__ == '__main__':
    unittest.main()
