"""Projective samples in the readable-tile version (dmsh_fetch.py): a sample at in.m_X.xy / in.m_X.w is rewritten like one at an interpolated output and marked 'proj', in its
spellings (a product with 1.0 / w either way round, a quotient) and not when a name is assigned twice or the components come from different outputs; screen_varyings finds the
clip → screen varying ((x + w) / 2, (w − y) / 2, ·, w) of gl_Position in every vertex entry point, after branches, and not with a flipped y, another w, an assignment in a branch,
an early return or a uniform coefficient; a sample at gl_FragCoord.xy [/ render scale] times a uniform is marked 'frag_pos' with its uniform references, and not at another
position, scale or uniform; the variants compile for the Mac and the iPhone. With --fixture <dir>, writes t_feedback.m's tile-read shaders: fb_vs.metal, fb_fs.metal, the variant
library fedcba9876543210_bf.metallib and facts.json (the vertex shader's screen varyings and the fragment shader's calls), and the pixel-position shader fb_pos.metal with its
variant fedcba9876543211_bf.metallib and pos.json"""
import json, pathlib, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_abi, dmsh_fetch

# Breath of the Wild's decal vertex shader 095cd6ad97fd779a as spirv-cross writes it, cut down (a full-screen triangle with w = 2 in place of the decal box), and a branch first,
# as Tears of the Kingdom's bb19fab2f950e587 has
VS = '''#include <metal_stdlib>
using namespace metal;

struct main0_out
{
    float4 m_45 [[user(locn4)]];
    float4 m_47 [[user(locn5)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    out.m_45 = float4(0.0, 0.0, 0.0, 1.0);
    out.m_47 = float4(0.0, 0.0, 0.0, 1.0);
    out.gl_Position.x = 0.0;
    out.gl_Position.y = 0.0;
    out.gl_Position.z = 0.0;
    out.gl_Position.w = 1.0;
    bool _81 = gl_VertexIndex == 1u;
    float _90 = -1.0;
    if (_81)
    {
        _90 = 3.0;
        out.m_47.x = 1.0;
    }
    float _92 = (gl_VertexIndex == 2u) ? 3.0 : -1.0;
    float _313 = 2.0;
    float _309 = _90 * _313;
    float _311 = _92 * _313;
    out.gl_Position.x = _309;
    out.gl_Position.y = _311;
    out.gl_Position.w = _313;
    out.m_45.w = _313;
    float _327 = _309 + _313;
    float _329 = -_311;
    float _331 = _329 + _313;
    float _361 = _327 * 0.5;
    float _363 = _331 * 0.5;
    out.m_45.x = _361;
    out.m_45.y = _363;
    out.m_47.w = _313;
    out.m_47.x = fma(out.gl_Position.x, 0.5, out.gl_Position.w * 0.5);
    out.m_47.y = fma(-out.gl_Position.y, 0.5, out.gl_Position.w * 0.5);
    out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);
    return out;
}
'''
# The decal fragment shader's G-buffer read (4d3d5a4408c2e8d9), its colours rotated into its output: green from blue, blue from red, red from green
FS = '''#include <metal_stdlib>
using namespace metal;

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_8 [[id(0)]];
    sampler fp_t_tcb_8Smplr [[id(1)]];
};

struct main0_out
{
    float4 m_46 [[color(0)]];
};

struct main0_in
{
    float4 m_35 [[user(locn4)]];
};

fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    float _57 = in.m_35.w;
    float _59 = in.m_35.x;
    float _61 = in.m_35.y;
    float _67 = 1.0 / _57;
    float _69 = _59 * _67;
    float _71 = _61 * _67;
    float2 _309 = float2(_69, _71);
    float4 _84 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, select(_309, float2(0.0), isnan(_309))).xyzw;
    out.m_46 = float4(_84.y, _84.z, _84.x, 1.0);
    return out;
}
'''
BINDINGS = [{'kind': dmsh_abi.KIND_TEX, 'msl_name': 'fp_t_tcb_8', 'reg': 0}]
FS_KEY = 'fedcba9876543210'
# Splatoon Raiders' ink stamp (6a5b76a60670e513), cut down: its ink map sampled at gl_FragCoord over the render scale times c[6].xy (1 / the map's size), through the border colour
# correction; the read's channels rotated into its output (red from green, green from blue plus one minus alpha): red where it read green, black where it read red, and green
# wherever a missing channel of an R or RG map read other than 0, 0, 1
FS_POS = """#include <metal_stdlib>
using namespace metal;

template<typename V, typename T, typename... A> static inline __attribute__((always_inline)) V dm_bs(V r, constant uint* b, uint i, T t, sampler w, A... a) { return r; }

struct _10
{
    float4 _m0[5];
    float4 _m5[1];
};

struct _16
{
    float4 _m0[16];
};

struct spvDescriptorSetBuffer0
{
    texture2d<float> fp_t_tcb_8 [[id(0)]];
    sampler fp_t_tcb_8Smplr [[id(1)]];
    constant _10* support_buffer [[id(2)]];
    constant _16* fp_c3 [[id(3)]];
    constant uint* dm_border [[id(4)]];
    sampler fp_t_tcb_8Smplr_w [[id(5)]];
};

struct main0_out
{
    float4 m_46 [[color(0)]];
};

fragment main0_out main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], float4 gl_FragCoord [[position]])
{
    main0_out out = {};
    float _43 = gl_FragCoord.x;
    float _45 = (*spvDescriptorSet0.support_buffer)._m5[0].x;
    float _47 = _43 / _45;
    float _49 = gl_FragCoord.y;
    float _51 = (*spvDescriptorSet0.support_buffer)._m5[0].x;
    float _53 = _49 / _51;
    float _55 = _47 * (*spvDescriptorSet0.fp_c3)._m0[6].x;
    float _57 = _53 * (*spvDescriptorSet0.fp_c3)._m0[6].y;
    float2 _286 = float2(_55, _57);
    float4 _59 = dm_bs(spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, select(_286, float2(0.0), isnan(_286))), spvDescriptorSet0.dm_border, 0u, spvDescriptorSet0.fp_t_tcb_8, spvDescriptorSet0.fp_t_tcb_8Smplr_w, select(_286, float2(0.0), isnan(_286))).xyzw;
    out.m_46 = float4(_59.y, _59.z + (1.0 - _59.w), _59.x, 1.0);
    return out;
}
"""
POS_KEY = 'fedcba9876543211'


def compile_lib(src, root, name, sdk='macosx'):   # → the library path; CalledProcessError with the compiler's message
    msl = root / (name + '.metal'); msl.write_text(src); air = root / (name + '.' + sdk + '.air'); lib = root / (name + '.metallib')
    for cmd in (['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + str(root / 'mc'), str(msl), '-o', str(air)], ['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(lib)]):
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode: raise subprocess.CalledProcessError(r.returncode, cmd, r.stdout, r.stderr)
    return lib


class ProjectiveSample(unittest.TestCase):
    def calls(self, src):
        v, meta = dmsh_fetch.variant(src, BINDINGS)
        return meta if v is None else meta['calls']

    def test_product_with_reciprocal(self):
        self.assertEqual(self.calls(FS), [{'fc': 0, 'tex_reg': 0, 'varying_locn': 4, 'proj': 1}])

    def test_reciprocal_first_and_quotient(self):
        self.assertEqual(self.calls(FS.replace('_59 * _67', '_67 * _59'))[0].get('proj'), 1)
        self.assertEqual(self.calls(FS.replace('float _69 = _59 * _67;', 'float _69 = _59 / _57;').replace('float _71 = _61 * _67;', 'float _71 = _61 / _57;'))[0].get('proj'), 1)

    def test_rewritten_like_a_plain_sample(self):
        v, _ = dmsh_fetch.variant(FS, BINDINGS)
        self.assertIn('(FX_0 == 0 ? fb0 : ', v)
        self.assertIn('float4 fb0 [[color(0), function_constant(FB_0)]]', v)

    def test_not_rewritten(self):
        for why, src in (('a name assigned twice', FS.replace('    float2 _309', '    _69 = 0.5;\n    float2 _309')),
                         ('w from another output', FS.replace('float _57 = in.m_35.w;', 'float _57 = in.m_35.z;')),
                         ('x from another output', FS.replace('float4 m_35 [[user(locn4)]];', 'float4 m_35 [[user(locn4)]];\n    float4 m_36 [[user(locn5)]];').replace('float _59 = in.m_35.x;', 'float _59 = in.m_36.x;')),
                         ('not divided by w', FS.replace('float _67 = 1.0 / _57;', 'float _67 = 2.0 / _57;'))):
            with self.subTest(why=why): self.assertEqual(self.calls(src), 'no rewritable sample')

    def test_compiles_on_both_platforms(self):
        v, _ = dmsh_fetch.variant(FS, BINDINGS)
        with tempfile.TemporaryDirectory() as d:
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    try: compile_lib(v, pathlib.Path(d), 'fs_' + sdk, sdk)
                    except subprocess.CalledProcessError as e: self.fail(e.stderr)


class PixelPositionSample(unittest.TestCase):
    def calls(self, src):
        v, meta = dmsh_fetch.variant(src, BINDINGS)
        return meta if v is None else meta['calls']

    def test_over_the_render_scale(self):
        self.assertEqual(self.calls(FS_POS), [{'fc': 0, 'tex_reg': 0, 'frag_pos': [[0, 6, 0], [0, 6, 1]]}])

    def test_without_the_render_scale_and_uniform_first(self):
        src = FS_POS.replace('    float _47 = _43 / _45;\n', '').replace('_47 * (*spvDescriptorSet0.fp_c3)._m0[6].x', '(*spvDescriptorSet0.fp_c3)._m0[6].x * _43')
        self.assertEqual(self.calls(src)[0]['frag_pos'], [[0, 6, 0], [0, 6, 1]])

    def test_not_rewritten(self):
        for why, src in (('y from x', FS_POS.replace('float _49 = gl_FragCoord.y;', 'float _49 = gl_FragCoord.x;')),
                         ('divided by a uniform', FS_POS.replace('float _45 = (*spvDescriptorSet0.support_buffer)._m5[0].x;', 'float _45 = (*spvDescriptorSet0.fp_c3)._m0[1].x;')),
                         ('a constant scale', FS_POS.replace('_47 * (*spvDescriptorSet0.fp_c3)._m0[6].x', '_47 * 0.25')),
                         ('a driver constant bank', FS_POS.replace('(*spvDescriptorSet0.fp_c3)._m0[6].x', '(*spvDescriptorSet0.fp_c2)._m0[6].x')),
                         ('an offset', FS_POS.replace('float2 _286 = float2(_55, _57);', 'float _58 = _55 + 0.5;\n    float2 _286 = float2(_58, _57);')),
                         ('a name assigned twice', FS_POS.replace('    float2 _286', '    _55 = 0.5;\n    float2 _286'))):
            with self.subTest(why=why): self.assertEqual(self.calls(src), 'no rewritable sample')

    def test_compiles_on_both_platforms(self):
        v, _ = dmsh_fetch.variant(FS_POS, BINDINGS)
        with tempfile.TemporaryDirectory() as d:
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    try: compile_lib(v, pathlib.Path(d), 'pos_' + sdk, sdk)
                    except subprocess.CalledProcessError as e: self.fail(e.stderr)


class ScreenVaryings(unittest.TestCase):
    def test_both_spellings_after_a_branch(self):
        self.assertEqual(dmsh_fetch.screen_varyings(VS), [4, 5])

    def test_every_entry_point(self):
        z = VS.split('vertex main0_out main0(')[1].replace('out.m_45.x = _361;', 'out.m_45.x = _363;')
        self.assertEqual(dmsh_fetch.screen_varyings(VS + '\nvertex main0_out main0_z(' + z), [5])

    def test_not_a_screen_varying(self):
        for why, src in (('y not flipped', VS.replace('float _329 = -_311;', 'float _329 = _311;')),
                         ('another w', VS.replace('out.m_45.w = _313;', 'out.m_45.w = 1.0;')),
                         ('assigned in a branch', VS.replace('    out.m_45.y = _363;\n', '    if (_81)\n    {\n        out.m_45.y = _363;\n    }\n')),
                         ('a uniform coefficient', VS.replace('float _361 = _327 * 0.5;', 'float _361 = _327 * spvDescriptorSet0.s;')),
                         ('a position read before its final value', VS.replace('float _327 = _309 + _313;', 'float _327 = out.gl_Position.x + _313;').replace('    out.gl_Position.x = _309;\n', '').replace('    out.m_45.x = _361;\n', '    out.m_45.x = _361;\n    out.gl_Position.x = _309;\n'))):
            with self.subTest(why=why): self.assertNotIn(4, dmsh_fetch.screen_varyings(src))
        early = VS.replace('    float _92', '    if (_81)\n    {\n        return out;\n    }\n    float _92')
        self.assertEqual(dmsh_fetch.screen_varyings(early), [], 'an early return')


def fixture(out):
    out = pathlib.Path(out); out.mkdir(parents=True, exist_ok=True)
    v, meta = dmsh_fetch.variant(FS, BINDINGS)
    (out / 'fb_vs.metal').write_text(VS); (out / 'fb_fs.metal').write_text(FS)
    compile_lib(v, out, 'v').rename(out / f'{FS_KEY}_bf.metallib')
    (out / 'facts.json').write_text(json.dumps({'screen': dmsh_fetch.screen_varyings(VS), 'calls': meta['calls'], 'outputs': meta['outputs']}))
    v, meta = dmsh_fetch.variant(FS_POS, BINDINGS)
    (out / 'fb_pos.metal').write_text(FS_POS)
    compile_lib(v, out, 'vp').rename(out / f'{POS_KEY}_bf.metallib')
    (out / 'pos.json').write_text(json.dumps({'calls': meta['calls'], 'outputs': meta['outputs']}))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--fixture': fixture(sys.argv[2])
    else: unittest.main()
