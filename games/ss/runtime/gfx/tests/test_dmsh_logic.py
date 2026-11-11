"""The logic operation library (dmsh_logic.py): the entry points (main0, main0_u, main0_m) read each colour output's attachment and combine it with the output before each return,
integer outputs bit for bit and float outputs through their normalized target's bits, the output stays a library that compiles for the Mac and the iPhone, and shaders without colour
outputs get none. With --fixture <dir>, writes the replay test's shaders (t_logic.m): the base source and its logic operation library, compiled"""
import pathlib, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi, dmsh_logic
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # its sibling test module, also when the file runs alone (python -m unittest <file>, as CI does)
from test_dmsh_alpha import compile_lib   # noqa: E402

BASE = '''#include <metal_stdlib>
using namespace metal;
struct spvDescriptorSetBuffer0 { constant uint4* value [[id(0)]]; };

static inline float4 helper(float4 v) { return v; }

struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.value));
    out.m_1 = float4(0.5);
    return out;
}

struct main0_out_u
{
    uint4 m_0 [[color(0)]];
    uint4 m_1 [[color(1)]];
};

fragment main0_out_u main0_u(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.value));
    if (gl_FragCoord.y < 0.0)
        return main0_out_u{as_type<uint4>(out.m_0), as_type<uint4>(out.m_1)};
    out.m_1 = float4(0.5);
    return main0_out_u{as_type<uint4>(out.m_0), as_type<uint4>(out.m_1)};
}

struct main0_out_m
{
    uint4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
};

fragment main0_out_m main0_m(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.value));
    out.m_1 = float4(0.5);
    return main0_out_m{as_type<uint4>(out.m_0), out.m_1};
}
'''
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'
STATE = f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER})]]'
FB0 = 'uint4 dm_fb0 [[color(0), function_constant(dm_lfb0)]]'


class LogicVariant(unittest.TestCase):
    def test_integer_entry_points_and_returns(self):
        v, entries = dmsh_logic.variant(BASE)
        self.assertEqual(entries, ['main0', 'main0_u', 'main0_m'])
        self.assertIn('[[buffer(0)]], ' + FB0 + ', uint4 dm_fb1 [[color(1), function_constant(dm_lfb1)]], ' + STATE + ')', v)   # main0_u: both outputs are integers
        self.assertIn('[[buffer(0)]], ' + FB0 + ', float4 dm_fb1 [[color(1), function_constant(dm_lfb1)]], ' + STATE + ')', v)   # main0_m: only colour 0 is
        for n in (0, 1): self.assertEqual(v.count(f'constant bool dm_lfb{n} [[function_constant({dmsh_abi.LOGIC_FC + n})]];'), 1)
        self.assertEqual(v.count('if (dm_lfb0) dm_r.m_0 = uint4(dm_logic(uint4(dm_r.m_0), dm_fb0, dm_lop, 0u));'), 3)   # main0_u's two returns, main0_m's one
        self.assertEqual(v.count('if (dm_lfb1) dm_r.m_1 = uint4(dm_logic(uint4(dm_r.m_1), dm_fb1, dm_lop, 1u));'), 2)
        self.assertIn('if (gl_FragCoord.y < 0.0)\n        { main0_out_u dm_r = main0_out_u{', v)   # a single-statement if keeps its return

    def test_float_outputs_go_through_the_targets_bits(self):
        v, entries = dmsh_logic.variant(BASE.split('\nstruct main0_out_u')[0])
        self.assertEqual(entries, ['main0'])
        self.assertIn('float4 dm_fb0 [[color(0), function_constant(dm_lfb0)]], float4 dm_fb1 [[color(1), function_constant(dm_lfb1)]], ' + STATE + ')', v)
        self.assertEqual(v.count('if (dm_lfb0) dm_r.m_0 = float4(dm_logic_norm(float4(dm_r.m_0), dm_fb0, dm_lop, 0u));'), 1)

    def test_only_the_entry_points_the_host_takes(self):   # the dual-source entry point stays out: a logic operation takes the place of blending
        dual = BASE + '\nstruct main0_out_d\n{\n    float4 m_0 [[color(0), index(0)]];\n    float4 m_1 [[color(0), index(1)]];\n};\n\nfragment main0_out_d main0_d(float4 gl_FragCoord [[position]])\n{\n    return main0_out_d{float4(0.0), float4(1.0)};\n}\n'
        v, entries = dmsh_logic.variant(dual)
        self.assertEqual(entries, ['main0', 'main0_u', 'main0_m']); self.assertNotIn('main0_d', v)

    def test_helper_after_the_shaders_own(self):
        v, _ = dmsh_logic.variant(BASE)
        self.assertLess(v.index('static inline float4 helper'), v.index('struct dm_logic_state'))
        self.assertLess(v.index('struct dm_logic_state'), v.index('constant bool dm_lfb0'))
        self.assertLess(v.index('constant bool dm_lfb1'), v.index('fragment main0_out_u main0_u('))

    def test_narrow_integer_outputs(self):
        v, _ = dmsh_logic.variant(BASE.replace('uint4 m_0 [[color(0)]];\n    uint4 m_1', 'uint m_0 [[color(0)]];\n    uint2 m_1').replace('as_type<uint4>(out.m_0), as_type<uint4>(out.m_1)', 'as_type<uint>(out.m_0.x), as_type<uint2>(out.m_1.xy)'))
        self.assertIn('if (dm_lfb0) dm_r.m_0 = uint(dm_logic(uint4(dm_r.m_0, 0u, 0u, 0u), dm_fb0, dm_lop, 0u).x);', v)
        self.assertIn('if (dm_lfb1) dm_r.m_1 = uint2(dm_logic(uint4(dm_r.m_1, 0u, 0u), dm_fb1, dm_lop, 1u).xy);', v)

    def test_narrow_float_outputs(self):
        v, _ = dmsh_logic.variant(BASE.split('\nstruct main0_out_u')[0].replace('float4 m_0 [[color(0)]];\n    float4 m_1', 'float m_0 [[color(0)]];\n    float2 m_1').replace('out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.value));', 'out.m_0 = 0.5;').replace('out.m_1 = float4(0.5);', 'out.m_1 = float2(0.5);'))
        self.assertIn('if (dm_lfb0) dm_r.m_0 = float(dm_logic_norm(float4(dm_r.m_0, 0.0f, 0.0f, 0.0f), dm_fb0, dm_lop, 0u).x);', v)
        self.assertIn('if (dm_lfb1) dm_r.m_1 = float2(dm_logic_norm(float4(dm_r.m_1, 0.0f, 0.0f), dm_fb1, dm_lop, 1u).xy);', v)

    def test_no_colour_output_no_library(self):
        depth_only = BASE.split('\nstruct main0_out_u')[0].replace('float4 m_0 [[color(0)]];\n    float4 m_1 [[color(1)]];', 'float gl_FragDepth [[depth(any)]];').replace('    out.m_0 = helper(as_type<float4>(*spvDescriptorSet0.value));\n    out.m_1 = float4(0.5);\n', '    out.gl_FragDepth = 0.5;\n')
        self.assertEqual(dmsh_logic.variant(depth_only), (None, 'no colour output'))

    def test_taken_buffer_or_fetch_no_library(self):
        self.assertEqual(dmsh_logic.variant(BASE.replace('[[buffer(0)]])\n{\n    main0_out out = {};\n    out.m_0 = helper', f'[[buffer(0)]], constant float& x [[buffer({dmsh_abi.LOGIC_BUFFER})]])\n{{\n    main0_out out = {{}};\n    out.m_0 = helper', 1))[0], None)
        self.assertEqual(dmsh_logic.variant(BASE.replace('float4 gl_FragCoord [[position]],', 'float4 gl_FragCoord [[position]], float4 fb [[color(0)]],', 1))[0], None)

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_compiles_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as d:
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    try: compile_lib(dmsh_logic.variant(BASE)[0], pathlib.Path(d), 'logic_' + sdk, sdk)
                    except subprocess.CalledProcessError as e: self.fail(e.stderr)


def fixture(out):   # t_logic.m's shaders: base.metal (the base, compiled by the test) and <key>_bl.metallib (the logic operation library the host loads), for key 1…1
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    (out / 'base.metal').write_text(BASE + VERTEX)
    compile_lib(dmsh_logic.variant(BASE)[0] + VERTEX, out, 'l').rename(out / ('1' * 16 + '_bl.metallib'))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
