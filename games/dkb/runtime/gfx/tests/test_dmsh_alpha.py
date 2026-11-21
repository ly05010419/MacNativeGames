"""The alpha test library (dmsh_alpha.py): every fragment entry point takes the state and tests colour 0's alpha before each return, the output stays a library that
compiles for the Mac and the iPhone, and shaders without a float colour 0 alpha get none. With --fixture <dir>, writes the replay test's shaders (t_alpha.m): the base
sources and their alpha test libraries, compiled"""
import pathlib, re, shutil, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi, dmsh_alpha

BASE = '''#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 m_0 [[color(0)]];
    float4 m_1 [[color(1)]];
};

fragment main0_out main0(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    out.m_0 = float4(1.0, 0.0, 0.0, gl_FragCoord.x / 8.0);
    if (gl_FragCoord.y < 0.0)
        return out;
    out.m_1 = out.m_0;
    return out;
}

struct main0_out_u
{
    uint4 m_0 [[color(0)]];
    uint4 m_1 [[color(1)]];
};

fragment main0_out_u main0_u(float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    out.m_0 = float4(1.0, 0.0, 0.0, gl_FragCoord.x / 8.0);
    return main0_out_u{as_type<uint4>(out.m_0), as_type<uint4>(out.m_1)};
}
'''
VERTEX = '\nvertex float4 vertex_main(uint i [[vertex_id]]) { float2 v[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)}; return float4(v[i], 0, 1); }\n'
ARGS = BASE.split('\nstruct main0_out_u')[0].replace('fragment main0_out main0(float4 gl_FragCoord [[position]])',
    'struct spvDescriptorSetBuffer0 { constant float* width [[id(0)]]; };\n\nfragment main0_out main0(float4 gl_FragCoord [[position]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])').replace('gl_FragCoord.x / 8.0', 'gl_FragCoord.x / *spvDescriptorSet0.width')
TEST = 'if (!dm_alpha_pass(float(out.m_0.w), dm_alpha)) discard_fragment();'
PARAM = f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER})]]'


def compile_lib(src, root, name, sdk='macosx'):   # → the library path; CalledProcessError with the compiler's message
    msl = root / (name + '.metal'); msl.write_text(src); air = root / (name + '.' + sdk + '.air'); lib = root / (name + '.metallib')
    for cmd in (['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / 'mc'), str(msl), '-o', str(air)], ['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: raise subprocess.CalledProcessError(r.returncode, cmd, r.stdout, r.stderr)
    return lib


class AlphaVariant(unittest.TestCase):
    def test_every_entry_point_and_return(self):
        v, entries = dmsh_alpha.variant(BASE)
        self.assertEqual(entries, ['main0', 'main0_u'])
        self.assertEqual(v.count(PARAM), 2)
        self.assertEqual(v.count(TEST), 3)   # main0's two returns, main0_u's one, which copies out after the test
        self.assertIn('{ ' + TEST + ' return main0_out_u{', v)

    def test_single_statement_if_keeps_its_return(self):
        v, _ = dmsh_alpha.variant(BASE)
        self.assertIn('if (gl_FragCoord.y < 0.0)\n        { ' + TEST + ' return out; }', v)

    def test_parameter_after_attributes_with_parentheses(self):
        v, _ = dmsh_alpha.variant(ARGS)
        self.assertIn('[[buffer(0)]], ' + PARAM + ')', v)

    def test_early_fragment_tests_kept(self):
        v, entries = dmsh_alpha.variant(BASE.replace('fragment main0_out main0(', '[[early_fragment_tests]] fragment main0_out main0('))
        self.assertEqual(entries, ['main0', 'main0_u'])
        self.assertIn('[[early_fragment_tests]] fragment main0_out main0(float4 gl_FragCoord [[position]], ' + PARAM + ')', v)

    def test_early_fragment_tests_spaced(self):   # SPIR-V Cross's spelling
        v, entries = dmsh_alpha.variant(BASE.replace('fragment main0_out main0(', '[[ early_fragment_tests ]] fragment main0_out main0('))
        self.assertEqual(entries, ['main0', 'main0_u'])
        self.assertIn('[[ early_fragment_tests ]] fragment main0_out main0(float4 gl_FragCoord [[position]], ' + PARAM + ')', v)

    def test_half_alpha(self):
        v, _ = dmsh_alpha.variant(BASE.replace('float4 m_0 [[color(0)]]', 'half4 m_0 [[color(0)]]'))
        self.assertIn('float(out.m_0.w)', v)

    def test_no_float_alpha_no_library(self):
        for src, why in ((BASE.replace('[[color(0)]]', '[[color(2)]]'), 'no colour 0 output'), (BASE.replace('float4 m_0 [[color(0)]]', 'uint4 m_0 [[color(0)]]'), 'colour 0 has no float alpha'),
                         (BASE.replace('float4 m_0 [[color(0)]]', 'float3 m_0 [[color(0)]]'), 'colour 0 has no float alpha'), (BASE.split('struct main0_out')[0], 'no colour output')):
            with self.subTest(why=why): self.assertEqual(dmsh_alpha.variant(src), (None, why))

    def test_taken_buffer_no_library(self):
        self.assertEqual(dmsh_alpha.variant(BASE.replace('float4 gl_FragCoord [[position]])', f'float4 gl_FragCoord [[position]], constant float& x [[buffer({dmsh_abi.ALPHA_BUFFER})]])', 1))[0], None)

    @unittest.skipUnless(shutil.which('xcrun'), 'requires Metal compilers')
    def test_compiles_on_both_platforms(self):
        with tempfile.TemporaryDirectory() as d:
            for name, src in (('base', BASE), ('args', ARGS), ('early', BASE.replace('fragment main0_out main0(', '[[ early_fragment_tests ]] fragment main0_out main0('))):
                for sdk in ('macosx', 'iphoneos'):
                    with self.subTest(shader=name, sdk=sdk):
                        try: compile_lib(dmsh_alpha.variant(src)[0], pathlib.Path(d), name + '_' + sdk, sdk)
                        except subprocess.CalledProcessError as e: self.fail(e.stderr)


def fixture(out):   # t_alpha.m's shaders: <name>.metal (the base, compiled by the test) and <key>_ba.metallib (the alpha test library the host loads), for key 0 and 1
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    for i, src in enumerate((BASE.split('\nstruct main0_out_u')[0] + '\n', ARGS)):
        (out / f'base{i}.metal').write_text(src + VERTEX)
        lib = compile_lib(dmsh_alpha.variant(src)[0] + VERTEX, out, f'a{i}')
        lib.rename(out / f'{str(i) * 16}_ba.metallib')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
