"""Synthetic geometry lowering, compiler and optional native GPU regressions."""
import ast
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import dmsh_geometry, dmsh_mesh

RASTER_CHECK = r'''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
int main(int argc, const char **argv) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "no Metal device\n"); return 77; }
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pipeline = [MTLRenderPipelineDescriptor new];
    pipeline.vertexFunction = [library newFunctionWithName:@(argv[2])];
    pipeline.fragmentFunction = [library newFunctionWithName:@"pixel"];
    pipeline.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:pipeline error:&error];
    if (!state) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    uint32_t masks[] = {0, 1, 2, 3, 0x80000000u, 0xffffffffu};
    for (unsigned clamp = 0; clamp < 2; clamp++)
    for (uint32_t points = 0; points < 2; points++)
    for (unsigned i = 0; i < sizeof(masks) / sizeof(*masks); i++) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor new];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:state];
        [encoder setDepthClipMode:clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
        [encoder setVertexBytes:&masks[i] length:sizeof(masks[i]) atIndex:0];
        [encoder setVertexBytes:&points length:sizeof(points) atIndex:1];
        [encoder drawPrimitives:points ? MTLPrimitiveTypePoint : MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:points ? 1 : 3];
        [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
        if (command.error) { fprintf(stderr, "%s\n", command.error.description.UTF8String); return 1; }
        uint8_t pixel[4];
        [texture getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(8, 8, 1, 1) mipmapLevel:0];
        if ((pixel[0] != 0) != ((masks[i] & 1u) != 0)) {
            fprintf(stderr, "mask %#x, points=%u, depth clamp=%u: red=%u\n", masks[i], points, clamp, pixel[0]); return 1;
        }
    }
    return 0;
} }
'''


def mask_reader():
    # GenerateVertexPassthroughForCompute treats ViewportMask (int[1]) as a
    # scalar output. SPIRV-Cross emits this invalid array-copy expression.
    return '''#include <metal_stdlib>
using namespace metal;
template<typename T, uint N>
inline void spvArrayCopyFromStackToStack(thread T (&dst)[N], thread const T (&src)[N])
{ for (uint i = 0; i < N; i++) dst[i] = src[i]; }
struct main0_out
{
    float4 gl_Position [[position]];
    float gl_PointSize [[point_size]];
};
vertex main0_out main0(uint vid [[vertex_id]], constant uint& mask [[buffer(0)]], constant uint& points [[buffer(1)]])
{
    main0_out out = {};
    float2 positions[] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    out.gl_Position = float4(points ? float2(0.0) : positions[vid], 0.0, 1.0);
    out.gl_PointSize = 64.0;
    float bits = as_type<float>(mask);
    spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);
    return out;
}
fragment float4 pixel() { return float4(1.0, 0.0, 0.0, 1.0); }
'''


def depth_entries(src):
    # Exercise the same post-reader depth conversion as geometry_finish without
    # importing the builder's command-line and game-profile initialization.
    builder = Path(dmsh_geometry.__file__).with_name('dmsh_build.py').read_text()
    node = next(n for n in ast.parse(builder).body if isinstance(n, ast.FunctionDef) and n.name == 'vs_depth')
    ns = {'re': re, 'sys': sys}
    exec(ast.get_source_segment(builder, node), ns)
    # The production helper locates the translated vertex entry at file end.
    vertex, pixel = src.split('fragment float4 pixel()', 1)
    return ns['vs_depth'](vertex)[1] + 'fragment float4 pixel()' + pixel


class GeometryReader(unittest.TestCase):
    def test_capture_loads_preserve_input_reads(self):
        source = 'device _22* vertex_output [[id(1)]]; device _22* geometry_vb_output [[id(2)]];'
        self.assertEqual(dmsh_geometry.capture_input(source),
                         'device volatile _22* vertex_output [[id(1)]]; device _22* geometry_vb_output [[id(2)]];')
        self.assertEqual(dmsh_geometry.capture_input('kernel void main0() {}'), 'kernel void main0() {}')
        with self.assertRaisesRegex(ValueError, 'capture-input declaration'):
            dmsh_geometry.capture_input('unknown vertex_output;')

    def test_ordinary_reader_is_unchanged(self):
        src = mask_reader().replace('    spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);\n', '')
        self.assertEqual(dmsh_geometry.reader(src), src)

    def test_unknown_mask_expression_is_rejected(self):
        for expression in ('gl_BuiltIn_5253[0] = 1;',
                           'spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits + 1.0).elements);'):
            with self.subTest(expression=expression), self.assertRaisesRegex(ValueError, 'viewport-mask expression'):
                dmsh_geometry.reader(mask_reader().replace(
                    'spvArrayCopyFromStackToStack(gl_BuiltIn_5253, as_type<int>(bits).elements);', expression))

    @unittest.skipUnless(sys.platform == 'darwin' and shutil.which('xcrun'), 'requires Metal on macOS')
    def test_mask_controls_viewport_zero_visibility(self):
        with tempfile.TemporaryDirectory(prefix='dmsh_geometry_raster_') as d:
            root = Path(d)
            source = root / 'reader.metal'; source.write_text(depth_entries(dmsh_geometry.reader(mask_reader())))
            library = root / 'reader.metallib'
            compiler = ['xcrun', '-sdk', 'macosx', 'metal', '-fmodules-cache-path=' + d + '/modules']
            r = subprocess.run([*compiler, '-c', str(source), '-o', str(root / 'reader.air')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(root / 'reader.air'), '-o', str(library)], check=True, capture_output=True)
            executable = root / 'raster'
            subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-x', 'objective-c', '-', '-o', str(executable),
                            '-framework', 'Foundation', '-framework', 'Metal'], input=RASTER_CHECK,
                           text=True, check=True, capture_output=True)
            for entry in ('main0', 'main0_z'):
                with self.subTest(entry=entry):
                    r = subprocess.run([str(executable), str(library), entry], capture_output=True, text=True)
                    if r.returncode == 77:
                        self.skipTest(r.stderr.strip())
                    self.assertEqual(r.returncode, 0, r.stderr)

    @unittest.skipUnless(shutil.which('xcrun'), 'requires the Metal compiler')
    def test_viewport_mask_reader_compiles(self):
        with tempfile.TemporaryDirectory(prefix='dmsh_geometry_test_') as d:
            src = Path(d) / 'reader.metal'
            src.write_text(dmsh_geometry.reader(mask_reader()))
            for sdk in ('macosx', 'iphoneos'):
                with self.subTest(sdk=sdk):
                    r = subprocess.run(['xcrun', '-sdk', sdk, 'metal',
                                        '-fmodules-cache-path=' + d + '/modules', '-fsyntax-only', str(src)],
                                       capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)


def geometry_header(passthrough=True, vertices=1, topology=7, invocations=1):
    return struct.pack('<5I', int(passthrough) << 24, 0, invocations << 24, topology << 24, vertices)


class GeometryOutputLayout(unittest.TestCase):
    def test_passthrough_uses_input_primitive_vertex_count(self):
        layouts = [dmsh_geometry.output_layout(geometry_header(), t) for t in range(5)]
        self.assertEqual([g['vertices'] for g in layouts], [1, 2, 2, 3, 3])
        self.assertEqual([g['index_stride'] for g in layouts], [2, 4, 4, 6, 6])
        self.assertEqual([g['output'] for g in layouts], [1, 6, 6, 7, 7])

    def test_regular_geometry_retains_declared_bound(self):
        for t in range(5):
            self.assertEqual(dmsh_geometry.output_layout(geometry_header(False, 12, 6, 3), t),
                             {'vertices': 12, 'invocations': 3, 'output': 6, 'index_stride': 24})

    def test_invalid_header_or_topology_is_rejected(self):
        for code, t in ((b'', 0), (geometry_header(), 5), (geometry_header(), -1),
                        (geometry_header(False, vertices=0), 0), (geometry_header(invocations=0), 0),
                        (geometry_header(False, topology=3), 0)):
            with self.subTest(code=code, topology=t), self.assertRaises(ValueError):
                dmsh_geometry.output_layout(code, t)

    @unittest.skipUnless(os.environ.get('DM_TEST_GEOMETRY'), 'set DM_TEST_GEOMETRY to the native t_geometry executable')
    def test_native_passthrough_canaries_and_compaction(self):
        layouts = [dmsh_geometry.output_layout(geometry_header(), t) for t in range(5)]
        with tempfile.TemporaryDirectory(prefix='geometry_bounds_') as d:
            path = Path(d) / 'layouts.json'; path.write_text(json.dumps(layouts))
            r = subprocess.run([os.environ['DM_TEST_GEOMETRY'], '--passthrough-layouts', str(path)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class PassthroughCompiler(unittest.TestCase):
    def test_passthrough_emits_positions_and_generic_attributes(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))   # a sibling module: the CI runs each test file from the repository root
        from test_dmsh_reflect import REFLECT
        root = Path(__file__).resolve().parents[3]
        table = (root / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
        pattern = re.search(r'Add\("([01x]{64})", InstName\.Exit\s*,', table)[1]
        exit_instruction = int(pattern.replace('x', '0'), 2) | (7 << 16) | 15
        with tempfile.TemporaryDirectory(prefix='geometry_passthrough_') as d:
            work = Path(d); jobs = []
            for t in range(5):
                header = bytearray(0x50)
                struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1,
                                 0, 1 << 24, 7 << 24, 1)
                code = work / f'pass{t}.bin'; code.write_bytes(header + struct.pack('<QQ', 0, exit_instruction))
                jobs.append(dict(key=f'pass{t}', stage='gs', code=str(code), geometry_topology=t,
                                 tex={}, img={}, attr={}, fout={}, debug=True, reduced=True))
            job_file = work / 'jobs.json'; job_file.write_text(json.dumps(jobs))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(job_file), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            for t, job in enumerate(jobs):
                with self.subTest(topology=t):
                    spv = work / f'pass{t}.spv'; dmsh_geometry.flatten_sets(spv)
                    r = subprocess.run([str(Path(__file__).resolve().parents[1] / 'tools/spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers',
                                        '--msl-texture-buffer-native', str(spv)], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    src = dmsh_geometry.capture_input(dmsh_geometry.topology_pointer(r.stdout))
                    self.assertTrue('geometry_vb_output' in src, 'passthrough kernel discarded its raster output')
                    res = json.loads((work / f'pass{t}.res.json').read_text())
                    layout = dmsh_geometry.output_layout((work / f'pass{t}.bin').read_bytes(), t)
                    self.assertEqual(res['geometry_stride'], 133)   # position, point size, all 32 float4 generic outputs
                    reflection = dict(route='B', bindings=REFLECT('synthetic', 'gs', src, res),
                                      geometry=dict(layout, stride=res['geometry_stride']))
                    (work / f'pass{t}.json').write_text(json.dumps(reflection))
                    metal = work / f'pass{t}.metal'; metal.write_text(src)
                    for sdk in ('macosx', 'iphoneos'):
                        air = work / f'pass{t}.{sdk}.air'
                        r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-fmodules-cache-path=' + d + '/modules',
                                            '-c', str(metal), '-o', str(air)], capture_output=True, text=True)
                        self.assertEqual(r.returncode, 0, r.stderr)
                        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(work / f'pass{t}.{sdk}.metallib')], check=True, capture_output=True)
            if os.environ.get('DM_TEST_GEOMETRY'):
                r = subprocess.run([os.environ['DM_TEST_GEOMETRY'], '--passthrough-kernels', d], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)



LEAN_KERNEL = '''struct _22 { float _m0[1]; };
struct spvDescriptorSetBuffer0
{
    device _22* vertex_output [[id(1)]];
    device _22* geometry_vb_output [[id(2)]];
    device uint* gp_s0 [[id(4)]];
};
kernel void main0(constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]], uint3 gl_GlobalInvocationID [[thread_position_in_grid]])
{
    spvUnsafeArray<float, 134> _41;
    int _10 = int(gl_GlobalInvocationID.x);
    int _12 = _10 * 144;
    int _14 = _12 + 36;
    float _16 = (*spvDescriptorSet0.vertex_output)._m0[_14];
    int _18 = _10 * 144;
    int _20 = _18 + 17;
    float _22 = (*spvDescriptorSet0.vertex_output)._m0[_20];
    _41[17] = _22;
    int _24 = _10 * 144;
    float _26 = (*spvDescriptorSet0.vertex_output)._m0[_24];
    _41[0] = _26;
    _41[5] = as_type<float>(as_type<uint>(_16) & 1u);
    uint _28 = atomic_fetch_add_explicit((device atomic_uint*)spvDescriptorSet0.gp_s0, 1u, memory_order_relaxed);
    int _30 = _10 * 134;
    float _32 = _41[0];
    (*spvDescriptorSet0.geometry_vb_output)._m0[_30] = _32;
    int _34 = _30 + 5;
    float _36 = _41[5];
    (*spvDescriptorSet0.geometry_vb_output)._m0[_34] = _36;
    int _38 = _30 + 17;
    float _40 = _41[17];
    (*spvDescriptorSet0.geometry_vb_output)._m0[_38] = _40;
}
'''


class LeanPassthrough(unittest.TestCase):
    def test_passthrough_bit(self):
        self.assertTrue(dmsh_geometry.passthrough(geometry_header()))
        self.assertFalse(dmsh_geometry.passthrough(geometry_header(False)))
        self.assertFalse(dmsh_geometry.passthrough(b''))

    def test_lean_keeps_mask_and_decision_reads(self):
        src, reads = dmsh_geometry.lean(LEAN_KERNEL, 134)
        self.assertEqual(reads, [36])   # word 17 and the position only fed the copy
        self.assertIn('int _12 = _10 * 1;', src)
        self.assertIn('int _14 = _12 + 0;', src)
        self.assertIn('int _34 = _10;', src)   # one mask word per output vertex
        self.assertEqual(src.count('geometry_vb_output)._m0['), 1)
        self.assertNotIn('_m0[_20]', src)
        self.assertNotIn('_m0[_24]', src)
        self.assertIn('atomic_fetch_add_explicit', src)   # storage writes stay even when unused

    def test_lean_rejects_unknown_shapes(self):
        cases = [('(*spvDescriptorSet0.geometry_vb_output)._m0[_34] = _36;', '(*spvDescriptorSet0.geometry_vb_output)._m0[_10 + 5] = _36;', 'unknown output store'),
                 ('int _14 = _12 + 36;', 'int _14 = _12 + _10;', 'computed capture read'),
                 ('int _34 = _30 + 5;', 'int _34 = _30 + 6;', 'no viewport mask store')]
        for old, new, message in cases:
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                dmsh_geometry.lean(LEAN_KERNEL.replace(old, new), 134)

    def test_capture_writes_each_word_once_through_its_slot(self):
        src = '''struct main0_out
{
    float4 m_3 [[user(locn3)]];
    float4 gl_Position [[position]];
};
vertex main0_out main0(uint vid [[vertex_id]])
{
    main0_out out = {};
    return out;
}
'''
        out = dmsh_geometry.capture(src)
        self.assertIn('short slot[144]', out)
        self.assertEqual(out.count('dm_cw(dm_gs, dm_p,'), 144)
        self.assertIn('dm_cw(dm_gs, dm_p, 0, as_type<uint>(out.gl_Position.x));', out)
        self.assertIn('dm_cw(dm_gs, dm_p, 28, as_type<uint>(out.m_3.x));', out)
        self.assertIn('dm_cw(dm_gs, dm_p, 4, 0x3f800000u);', out)   # point size
        self.assertIn('dm_cw(dm_gs, dm_p, 23, 0x3f800000u);', out)   # an unwritten attribute's w
        self.assertIn('dm_cw(dm_gs, dm_p, 20, 0u);', out)
        self.assertIn('* dm_gs.stride)', out)

MESH_VS = """#include <metal_stdlib>
using namespace metal;

struct spvDescriptorSetBuffer0
{
    constant float4* c [[id(0)]];
};

struct main0_out
{
    float4 m_1 [[user(locn5)]];
    float4 gl_Position [[position, invariant]];
    float gl_PointSize [[point_size]];
};

struct main0_in
{
    float4 m_0 [[attribute(0)]];
};

vertex main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_1 = in.m_0;
    out.gl_PointSize = 1.0;
    out.gl_Position = in.m_0 * spvDescriptorSet0.c[0];
    return out;
}

vertex main0_out main0_z(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])
{
    main0_out out = {};
    out.m_1 = in.m_0;
    out.gl_PointSize = 1.0;
    out.gl_Position = in.m_0;
    return out;
}
"""
MESH_HEAD = """#include <metal_stdlib>
using namespace metal;
template<typename T, size_t Num>
struct spvUnsafeArray
{
    T elements[Num ? Num : 1];
    thread T& operator [] (size_t pos) thread { return elements[pos]; }
};
"""


class MeshPassthrough(unittest.TestCase):
    def test_compose(self):
        lean, reads = dmsh_geometry.lean(LEAN_KERNEL, 134)
        src = dmsh_mesh.compose(MESH_VS, MESH_HEAD + '\n' + lean, reads)
        self.assertIn('[[mesh]] void dm_mesh(', src)
        self.assertIn('[[mesh]] void dm_mesh_z(', src)
        self.assertIn('dm_rec[dm_t * 1u + 0u] = as_type<float>(dm_o.m_1.x);', src)   # word 36 = location 5 x
        self.assertIn('dm_mask = as_type<uint>(_36);', src)
        self.assertNotIn('spvDescriptorSet0.vertex_output', src)
        self.assertNotIn('[[point_size]]', src)
        self.assertIn('dm_ix + uint(dm_d.base_vertex)', src)
        if shutil.which('xcrun') and sys.platform == 'darwin':
            with tempfile.TemporaryDirectory() as d:
                Path(d, 'm.metal').write_text(src)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-fmodules-cache-path=' + d + '/modules', str(Path(d, 'm.metal')), '-o', str(Path(d, 'm.air'))], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-2000:])

    def test_compose_rejects_clip_z(self):
        lean, _ = dmsh_geometry.lean(LEAN_KERNEL, 134)
        with self.assertRaisesRegex(ValueError, 'clip z'):
            dmsh_mesh.compose(MESH_VS, MESH_HEAD + lean, [2])

    @unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE') and shutil.which('xcrun'), 'requires the offline translator and Metal compiler')
    def test_translated_primitive_id_crosses_mesh_groups(self):
        root = Path(__file__).resolve().parents[3]
        table = (root / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
        def instruction(name, emitter=''):
            pattern = r'Add\("([01x]{64})",\s*InstName\.' + name + r'\s*,'
            if emitter: pattern += r'\s*InstEmit\.' + emitter + r'\s*,'
            return int(re.search(pattern, table)[1].replace('x', '0'), 2) | (7 << 16)
        # ALD R0, PrimitiveId; SHR R0, R0, 5; ALD R1, loc5.x;
        # IADD R0, R0, R1; AST ViewportMask, R0; EXIT.
        # loc5.x holds the primitive parity as raw integer bits. Check every
        # primitive's visibility: group parity plus captured vertex parity.
        ops = [instruction('Ald') | (255 << 8) | (255 << 39) | (0x60 << 20),
               instruction('Shr', 'ShrI') | (5 << 20),
               instruction('Ald') | 1 | (255 << 8) | (255 << 39) | (0xd0 << 20),
               instruction('Iadd', 'IaddR') | (1 << 20),
               instruction('Ast') | (255 << 8) | (0x3a0 << 20), instruction('Exit') | 15]
        header = bytearray(0x50)
        struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1, 0, 1 << 24, 7 << 24, 1)
        with tempfile.TemporaryDirectory(prefix='mesh_primitive_id_') as directory:
            work = Path(directory); code = work / 'pid.bin'
            code.write_bytes(header + struct.pack('<8Q', 0, *ops[:3], 0, *ops[3:]))
            job = dict(key='pid', stage='gs', code=str(code), geometry_topology=3,
                       tex={}, img={}, attr={}, fout={}, debug=True, reduced=True)
            (work / 'jobs.json').write_text(json.dumps([job]))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), directory], check=True, capture_output=True)
            spv = work / 'pid.spv'; dmsh_geometry.flatten_sets(spv)
            r = subprocess.run([str(root / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300',
                                '--msl-argument-buffers', '--msl-texture-buffer-native', str(spv)], check=True, capture_output=True, text=True)
            full = dmsh_geometry.topology_pointer(r.stdout)
            metadata = json.loads((work / 'pid.res.json').read_text())
            lean, reads = dmsh_geometry.lean(full, metadata['geometry_stride'])
            self.assertEqual(reads, [36])
            vertex = MESH_VS.replace('out.m_1 = in.m_0;', 'out.m_1 = float4(as_type<float>(uint(in.m_0.z)), 0, 0, 0);')
            vertex = vertex.replace('out.gl_Position = in.m_0', 'out.gl_Position = float4(in.m_0.xy, 0, 1)')
            source = dmsh_mesh.compose(vertex, lean, reads) + '\nfragment float4 mesh_pixel() { return float4(1, 0, 0, 1); }\n'
            metal = work / 'pid.metal'; metal.write_text(source)
            for sdk in ('macosx', 'iphoneos'):
                air = work / f'{sdk}.air'
                r = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + directory + '/modules',
                                    str(metal), '-o', str(air)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr[-3000:])
            if os.environ.get('DMSH_TEST_GPU') == '1':
                library = work / 'pid.metallib'
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(work / 'macosx.air'), '-o', str(library)], check=True, capture_output=True)
                binary = work / 'mesh_check'
                subprocess.run(['xcrun', 'clang', '-fobjc-arc', str(root / 'dm/gfx/tests/mesh_primitive_id.m'),
                                '-framework', 'Foundation', '-framework', 'Metal', '-o', str(binary)], check=True, capture_output=True)
                r = subprocess.run([str(binary), str(library)], capture_output=True, text=True)
                if r.returncode == 77: self.skipTest('no Metal device')
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'requires the offline translator')
class ComputeSplitPrimitiveId(unittest.TestCase):
    def test_real_primitive_ids_survive_compute_splits(self):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from test_dmsh_reflect import REFLECT
        root = Path(__file__).resolve().parents[3]
        table = (root / 'dm/third_party/ryujinx/src/Ryujinx.Graphics.Shader/Decoders/InstTable.cs').read_text()
        def instruction(name, emitter=''):
            pattern = r'Add\("([01x]{64})",\s*InstName\.' + name + r'\s*,'
            if emitter: pattern += r'\s*InstEmit\.' + emitter + r'\s*,'
            return int(re.search(pattern, table)[1].replace('x', '0'), 2) | (7 << 16)
        # Valid passthrough GS: every primitive emits with viewport mask PID+1.
        # Feedback reads that mask and the captured vertex/instance attributes.
        ops = [instruction('Ald') | (255 << 8) | (255 << 39) | (0x60 << 20),
               instruction('Iadd', 'IaddI') | (1 << 20),
               instruction('Ast') | (255 << 8) | (0x3a0 << 20), instruction('Exit') | 15]
        with tempfile.TemporaryDirectory(prefix='compute_primitive_id_') as directory:
            work = Path(directory); jobs = []
            for t in range(5):
                for inv in (1, 3):
                    key = f'pid{t}_{inv}'; header = bytearray(0x50)
                    struct.pack_into('<5I', header, 0, (1 << 24) | (4 << 10) | (3 << 5) | 1, 0, inv << 24, 7 << 24, 1)
                    code = work / (key + '.bin')
                    code.write_bytes(header + struct.pack('<8Q', 0, *ops[:3], 0, ops[3], 0, 0))
                    jobs.append(dict(key=key, stage='gs', code=str(code), geometry_topology=t, tex={}, img={}, attr={}, fout={}, debug=True, reduced=True))
            (work / 'jobs.json').write_text(json.dumps(jobs))
            subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), directory], check=True, capture_output=True)
            sources = []
            for job in jobs:
                key = job['key']; spv = work / (key + '.spv'); dmsh_geometry.flatten_sets(spv)
                result = subprocess.run([str(root / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', '20300', '--msl-argument-buffers', '--msl-texture-buffer-native', str(spv)], check=True, capture_output=True, text=True)
                source = dmsh_geometry.capture_input(dmsh_geometry.topology_pointer(result.stdout))
                # ABI check also runs on CI hosts without a Metal device and
                # catches a stale translator DLL that omits the primitive base.
                self.assertTrue('(*spvDescriptorSet0.vb_info)._m1.y' in source,
                                'translated PrimitiveID must read the base from geometry_counts.y')
                metadata = json.loads((work / (key + '.res.json')).read_text())
                layout = dmsh_geometry.output_layout(Path(job['code']).read_bytes(), job['geometry_topology'])
                (work / (key + '.json')).write_text(json.dumps(dict(route='B', bindings=REFLECT(key, 'gs', source, metadata), geometry=dict(layout, stride=metadata['geometry_stride']))))
                path = work / (key + '.metal'); path.write_text(source); sources.append(path)
            vertex = """#include <metal_stdlib>
using namespace metal;
struct main0_out
{
    float4 m_0 [[user(locn0)]];
    float4 gl_Position [[position]];
};
vertex main0_out main0(uint v [[vertex_id]], uint i [[instance_id]])
{
    main0_out out = {};
    out.m_0 = as_type<float4>(uint4(v, i, v ^ i, 1));
    out.gl_Position = float4(0, 0, 0, 1);
    return out;
}
"""
            path = work / 'vertex.metal'; path.write_text(vertex + dmsh_geometry.capture(vertex)); sources.append(path)
            for path in sources if os.environ.get('DMSH_TEST_METAL') == '1' or os.environ.get('DMSH_TEST_GPU') == '1' else []:
                for sdk in ('macosx', 'iphoneos'):
                    air = path.with_suffix('.' + sdk + '.air')
                    result = subprocess.run(['xcrun', '-sdk', sdk, 'metal', '-c', '-fmodules-cache-path=' + directory + '/modules', str(path), '-o', str(air)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr[-3000:])
                    if sdk == 'macosx':
                        subprocess.run(['xcrun', '-sdk', sdk, 'metallib', str(air), '-o', str(path.with_suffix('.metallib'))], check=True, capture_output=True)
            if os.environ.get('DMSH_TEST_GPU') == '1':
                binary = os.environ.get('DM_TEST_GEOMETRY_PRIMITIVE_ID')
                self.assertTrue(binary, 'set DM_TEST_GEOMETRY_PRIMITIVE_ID to the native replay test')
                result = subprocess.run([binary, directory], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                print(result.stdout.strip())


if __name__ == '__main__':
    unittest.main()
