"""Border colours Metal has no sampler state for (dmsh_build.py border_emu): the rewrite of a shader's samples, the reflection of the twins and the block, the readable-tile variant
of a rewritten shader, the compile on both platforms, and with DM_TEST_BORDER (the t_border executable) the samples on the GPU."""
import ast
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402
import dmsh_fetch   # noqa: E402
import dmsh_geometry   # noqa: E402

tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
names = {'border_emu', 'reflect', 'decode_tex', 'decode_ubo_tex', 'const_sampler', 'c2_array'}
selected = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in names)
            or (isinstance(n, ast.Assign) and any(getattr(e, 'id', None) in ('BORDER_HELPERS', 'SSBO', 'TEX_BASE') for t in n.targets for e in (t.elts if isinstance(t, ast.Tuple) else [t])))]
scope = {'re': re, 'abi': abi, 'dmsh_geometry': dmsh_geometry}
exec(compile(ast.Module(body=selected, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), scope)
border_emu, reflect = scope['border_emu'], scope['reflect']

HEAD = '#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n'
# Tears of the Kingdom's ff9def5b3c418851 as spirv-cross writes it, cut down: a uniform segment, the bend map and its sampler
GRASS = HEAD + '''struct _16
{
    float4 _m0[4096];
};

struct spvDescriptorSetBuffer0
{
    constant _16* fp_c1 [[id(0)]];
    texture2d<float> fp_t_tcb_8 [[id(1)]];
    sampler fp_t_tcb_8Smplr [[id(2)]];
    array<texture2d<float>, 4> fp_t2d_cb14_0 [[id(3)]];
    array<sampler, 4> fp_t2d_cb14_0Smplr [[id(7)]];
};

struct main0_out
{
    float4 m_32 [[color(0)]];
};

struct main0_in
{
    float4 m_29 [[user(locn0), center_no_perspective]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    float _39 = in.m_29.x;
    float _41 = in.m_29.y;
    float2 _182 = float2(_39, _41);
    float2 _45 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, select(_182, float2(0.0), isnan(_182))).xy;
    float4 _50 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, _182, level(0.0)).xy, bias(0.5));
    float4 _51 = spvDescriptorSet0.fp_t2d_cb14_0[1].sample(spvDescriptorSet0.fp_t2d_cb14_0Smplr[1], _182);
    out.m_32 = float4(_45, _50.x + _51.y, spvDescriptorSet0.fp_c1->_m0[0].x);
    return out;
}
'''
# what t_border runs: one texture and its sampler, sampled at explicit coordinates
KERNEL = HEAD + '''struct spvDescriptorSetBuffer0
{
    texture2d<float> cp_t_tcb_8 [[id(0)]];
    sampler cp_t_tcb_8Smplr [[id(1)]];
};

kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], device float4* out [[buffer(1)]], constant float2* uv [[buffer(2)]], uint i [[thread_position_in_grid]])
{
    out[i] = spvDescriptorSet0.cp_t_tcb_8.sample(spvDescriptorSet0.cp_t_tcb_8Smplr, uv[i], level(0.0));
}
'''

# an integer texture beside a float one, each with its sampler, and an image with an attribute after its id (SPIRV-Cross's interlock) as the last member
INTEGER = HEAD + '''struct spvDescriptorSetBuffer0
{
    texture2d<uint> fp_t_tcb_8 [[id(0)]];
    sampler fp_t_tcb_8Smplr [[id(1)]];
    texture2d<float> fp_t_tcb_9 [[id(2)]];
    sampler fp_t_tcb_9Smplr [[id(3)]];
    texture2d<float, access::read_write> fp_i_0 [[id(4), raster_order_group(0)]];
};

struct main0_out
{
    float4 m_32 [[color(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    uint4 _10 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, float2(0.5), level(0.0));
    float4 _11 = spvDescriptorSet0.fp_t_tcb_9.sample(spvDescriptorSet0.fp_t_tcb_9Smplr, float2(0.5));
    out.m_32 = float4(_10) + _11;
    return out;
}
'''


def members(msl):
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S).group(1)
    return [(m.group(1), m.group(2), int(m.group(3))) for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)\]\];', body, re.M)]


class Rewrite(unittest.TestCase):
    def test_members_follow_every_entry(self):
        got = members(border_emu(GRASS))
        self.assertEqual(got[-2:], [('sampler', 'fp_t_tcb_8Smplr' + abi.BORDER_TWIN, 11), ('constant dm_border_t*', abi.BORDER_MEMBER, 12)],
                         'the twin and the block come after the sampler array (ids 7-10)')

    def test_samples_are_wrapped(self):
        out = border_emu(GRASS)
        self.assertIn('dm_bs(spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, select(_182, float2(0.0), isnan(_182))), spvDescriptorSet0.dm_border, 0u, '
                      'spvDescriptorSet0.fp_t_tcb_8, spvDescriptorSet0.fp_t_tcb_8Smplr_w, select(_182, float2(0.0), isnan(_182))).xy', out)
        self.assertEqual(out.count('dm_bs('), 5, 'the helper, and four samples through the scalar sampler: one, one inside another (copied into its twin sample), the outer one')
        self.assertIn('spvDescriptorSet0.fp_t2d_cb14_0[1].sample(spvDescriptorSet0.fp_t2d_cb14_0Smplr[1], _182)', out, 'a sampler array keeps its samples')
        inner = 'dm_bs(spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, _182, level(0.0)), spvDescriptorSet0.dm_border, 0u, spvDescriptorSet0.fp_t_tcb_8, spvDescriptorSet0.fp_t_tcb_8Smplr_w, _182, level(0.0))'
        self.assertEqual(out.count(inner), 2, 'the inner sample, rewritten, in both of the outer one\'s samples')

    def test_integer_textures_keep_their_samples(self):
        out = border_emu(INTEGER)
        self.assertIn('uint4 _10 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, float2(0.5), level(0.0));', out, 'an integer sample is not wrapped')
        self.assertNotIn('fp_t_tcb_8Smplr' + abi.BORDER_TWIN, out, 'nor its sampler twinned')
        self.assertIn('dm_bs(spvDescriptorSet0.fp_t_tcb_9.sample(', out)

    def test_members_follow_an_attributed_member(self):
        body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', border_emu(INTEGER), re.S).group(1)
        self.assertIn('sampler fp_t_tcb_9Smplr' + abi.BORDER_TWIN + ' [[id(5)]];', body, 'after the image at id 4, which has raster_order_group')
        self.assertIn(abi.BORDER_MEMBER + ' [[id(6)]];', body)

    def test_unchanged_without_samples(self):
        src = HEAD + 'struct spvDescriptorSetBuffer0\n{\n    texture2d<float> fp_t_tcb_8 [[id(0)]];\n};\n\nfragment void main0() {}\n'
        self.assertEqual(border_emu(src), src)

    def test_reflection(self):
        b = {e['msl_name']: e for e in reflect('k', 'ps', border_emu(GRASS), {'sbufs': []})}
        base, twin = b['fp_t_tcb_8Smplr'], b['fp_t_tcb_8Smplr' + abi.BORDER_TWIN]
        self.assertEqual((twin['kind'], twin.get('reg'), twin['border_bit'], twin['arg_id']), (base['kind'], base.get('reg'), 0, 11))
        self.assertNotIn('border_bit', base)
        self.assertEqual((b[abi.BORDER_MEMBER]['kind'], b[abi.BORDER_MEMBER]['arg_id']), (abi.KIND_BORDER, 12))

    def test_readable_tile_variant_takes_the_wrapped_sample(self):
        plain = re.sub(r'    float4 _50 = .*\n', '    float4 _50 = float4(0.0);\n', GRASS.replace(', center_no_perspective', ''))   # a plain interpolated input, one sample left
        src = border_emu(plain)
        refl = reflect('k', 'ps', src, {'sbufs': []})
        fv, meta = dmsh_fetch.variant(src, refl)
        self.assertIsNotNone(fv, meta)
        self.assertEqual(len(meta['calls']), 1)
        self.assertEqual(meta['mixed_regs'], [])
        self.assertRegex(fv, r'\(FX_0 == 0 \? fb0 : [^;]*FX_0 == 7 \? fb7 : dm_bs\(spvDescriptorSet0\.fp_t_tcb_8\.sample\(', 'a tile read replaces the corrected sample')

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_compiles(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, src in (('grass', GRASS), ('kernel', KERNEL), ('integer', INTEGER)):
                msl = root / (name + '.metal'); msl.write_text(border_emu(src))
                for sdk in ('macosx', 'iphoneos'):
                    with self.subTest(shader=name, sdk=sdk):
                        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / sdk), str(msl), '-o', str(root / f'{name}_{sdk}.air')], capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(os.environ.get('DM_TEST_BORDER'), 'set DM_TEST_BORDER to the native t_border executable')
    def test_gpu(self):
        out = border_emu(KERNEL)
        self.assertEqual([n for _, n, _ in members(out)], ['cp_t_tcb_8', 'cp_t_tcb_8Smplr', 'cp_t_tcb_8Smplr' + abi.BORDER_TWIN, abi.BORDER_MEMBER], 't_border fills these four')
        with tempfile.TemporaryDirectory(prefix='border_') as directory:
            path = Path(directory) / 'kernel.metal'; path.write_text(out)
            r = subprocess.run([os.environ['DM_TEST_BORDER'], str(path)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
