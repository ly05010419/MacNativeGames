"""dmsh_build.py pool_arrays and reflect on synthetic MSL: Ryujinx's arrays for a handle traced to no slot, sized to the whole texture / sampler ID ranges (Program.cs), become pointers to
the pools the replay keeps (nvn_mtl.m pool_gpu), declared with one element and read through a pointer to it with the run-time bound; the argument IDs are renumbered as the host lays the buffer out; 32-entry slot arrays stay; members with
attributes after their id (raster_order_group) are read; a member or a use the rewrite cannot read stops the shader instead of leaving the host's argument buffer longer than the reflection."""
import ast
from pathlib import Path
import re
import sys
import types
import unittest

TOOLS = Path(__file__).resolve().parents[2] / 'gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_abi as abi   # noqa: E402

FUNCS = {'decode_tex', 'const_sampler', 'decode_ubo_tex', 'c2_array', 'pool_arrays', 'reflect'}
NAMES = {'POOL_MIN', 'TEX_BASE', 'SSBO'}


def load():
    tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
    nodes: list[ast.stmt] = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in FUNCS)
             or (isinstance(n, ast.Assign) and any(isinstance(t, (ast.Name, ast.Tuple)) and {getattr(e, 'id', None) for e in (t.elts if isinstance(t, ast.Tuple) else [t])} & NAMES for t in n.targets))]
    ns = {'re': re, 'abi': abi, 'dmsh_geometry': types.SimpleNamespace(HELPERS={})}
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), ns)
    assert FUNCS <= set(ns), FUNCS - set(ns)
    return ns


NS = load()
MSL = '''struct spvDescriptorSetBuffer0
{
    constant _10* support_buffer [[id(0)]];
    constant _16* fp_c5 [[id(1)]];
    array<texture2d<float>, 1048576> fp_t2d_cb0_40000000 [[id(2)]];
    array<sampler, 4096> fp_s_cb0_40000000 [[id(1048578)]];
    array<texture2d<float>, 32> fp_t2d_cb14_0 [[id(1052674)]];
    texture2d<uint> fp_i_cb2_48_rg32ui [[id(1052706), raster_order_group(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    float4 a = spvDescriptorSet0.fp_t2d_cb0_40000000[int(_9[2u])].sample(spvDescriptorSet0.fp_s_cb0_40000000[int(_7)], uv);
}
'''


class PoolArrays(unittest.TestCase):
    def test_rewrite(self):
        out = NS['pool_arrays'](MSL)
        self.assertIn('struct spvPool_fp_t2d_cb0_40000000\n{\n    uint n;\n    array<texture2d<float>, 1> e;\n};', out)
        self.assertIn('struct spvPool_fp_s_cb0_40000000\n{\n    uint n;\n    array<sampler, 1> e;\n};', out)
        self.assertLess(out.index('struct spvPool_'), out.index('struct spvDescriptorSetBuffer0'))
        ids = re.findall(r'(\w+) \[\[id\((\d+)\)', out)
        self.assertEqual(ids, [('support_buffer', '0'), ('fp_c5', '1'), ('fp_t2d_cb0_40000000', '2'), ('fp_s_cb0_40000000', '3'), ('fp_t2d_cb14_0', '4'), ('fp_i_cb2_48_rg32ui', '36')])
        self.assertIn('constant spvPool_fp_t2d_cb0_40000000* fp_t2d_cb0_40000000 [[id(2)]];', out)
        self.assertIn('fp_i_cb2_48_rg32ui [[id(36), raster_order_group(0)]];', out)
        self.assertIn('(&spvDescriptorSet0.fp_t2d_cb0_40000000->e[0])[min(uint(int(_9[2u])), spvDescriptorSet0.fp_t2d_cb0_40000000->n)]', out)
        self.assertIn('.sample((&spvDescriptorSet0.fp_s_cb0_40000000->e[0])[min(uint(int(_7)), spvDescriptorSet0.fp_s_cb0_40000000->n)], uv)', out)

    def test_slot_arrays_stay(self):
        src = MSL.replace('1048576', '32').replace('4096', '32')
        self.assertEqual(NS['pool_arrays'](src), src)

    def test_uncovered_use(self):
        with self.assertRaises(ValueError): NS['pool_arrays'](MSL.replace('    float4 a', '    f(spvDescriptorSet0.fp_t2d_cb0_40000000);\n    float4 a'))


class Reflect(unittest.TestCase):
    def test_pools_and_attributes(self):
        b = {e['msl_name']: e for e in NS['reflect']('k', 'ps', NS['pool_arrays'](MSL), {'sbufs': []})}
        self.assertEqual((b['fp_t2d_cb0_40000000']['kind'], b['fp_t2d_cb0_40000000']['elem'], b['fp_t2d_cb0_40000000']['arg_id']), (abi.KIND_TEX_POOL, 'texture2d<float>', 2))
        self.assertEqual((b['fp_s_cb0_40000000']['kind'], b['fp_s_cb0_40000000']['arg_id']), (abi.KIND_SAMP_POOL, 3))
        self.assertEqual((b['fp_i_cb2_48_rg32ui']['kind'], b['fp_i_cb2_48_rg32ui']['reg'], b['fp_i_cb2_48_rg32ui']['arg_id']), (abi.KIND_IMG, 0, 36))

    def test_unreadable_member(self):
        with self.assertRaises(ValueError): NS['reflect']('k', 'ps', MSL.replace('raster_order_group(0)', 'raster_order_group'), {'sbufs': []})


if __name__ == '__main__':
    unittest.main()
