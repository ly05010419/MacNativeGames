"""Hyrule Warriors: Age of Calamity on the Zelda-line runtime: the pieces this port added that run without the game.

- KTGL's shaders (scripts/ktgl_shaders.py): GLSLC outputs in the zlib-chunked IDRK entries of the RDB data files and in
  main, keyed as the runtime keys them, on synthetic files; the seed tool's game side (prepare_aocx_shader_seed.py);
- the rewriter (third_party/d2r/patch_nso.py, games/rdr's with its leading patch area): byte jump tables indexed
  through w30, which capstone names lr (main+0x13847dc), on a synthetic module; the leading layout as rdr's tests;
- the graphics layer's changes this game needed: a compute unbind keeps the graphics program, a buffer-to-texture copy
  owns the texture (no re-upload of the pool bytes), SetCopyImageStride, c1 segments beyond 4 KB (nvn_refl.m, compiled
  and run here);
- the host answers (SDK 9.6.0's four-argument nn::init::Start, the screenshot copyright overlay, ShowError(ErrorCode),
  GetNpadJoyHoldType, the host's VP9 movie decoder), the headless picture layer, the add-on content extraction;
- that the port carries no nxnative code and is built on the Zelda line (games/rdr's runtime and shared/)."""
import hashlib
import importlib.util
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
NVN = HERE / 'third_party/d2r_gfx/gfx/nvn'
sys.path.insert(0, str(HERE / 'tests'))
sys.path.insert(0, str(HERE / 'scripts'))
import synthetic_nso  # noqa: E402
import ktgl_shaders  # noqa: E402


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def rewriter_deps():
    try:
        import capstone  # noqa: F401
        import lz4.block  # noqa: F401
        return True
    except ImportError:
        return False


def fixups(folder, name):
    out = {}
    for line in (folder / f'{name}.fixups.tsv').read_text().splitlines():
        _, key, value = line.split()
        out[key] = int(value, 0)
    return out


def branch_target(word, at):
    assert word >> 26 == 0x05, f'not a B at {at:#x}: {word:#010x}'
    imm = word & 0x3ffffff
    return at + ((imm - (1 << 26)) if imm >> 25 else imm) * 4


@unittest.skipUnless(rewriter_deps(), 'capstone and lz4 (the rewriter\'s modules)')
def glslc_output(stage, body, key_len=0x880):
    """A GLSLC compiler output of one GPU code section: magic 0x19866891, its length at +0x44, one section header at
    +0x90 (size, data offset, type 0; +0x30 control offset, +0x34 code offset, +0x38 code length, +0x3c control length);
    the control section (0x98761234; c1 at +0x700, the code length at +0x704, the stage at +0x714) and the code (a 0x30
    header with 0x12345678, for vertex and fragment). Returns (bytes, the runtime's key)."""
    code = struct.pack('<I', 0x12345678) + bytes(0x2c) + body
    control = bytearray(key_len + 0x10)
    struct.pack_into('<I', control, 0, 0x98761234)
    struct.pack_into('<I', control, 0x20, key_len)
    struct.pack_into('<III', control, 0x700, 0x30, len(code), 0)
    struct.pack_into('<I', control, 0x714, stage)
    section = bytes(control) + code + bytes(0x20)   # a longer code area than the runtime hashes
    head = bytearray(0x90 * 2)
    struct.pack_into('<I', head, 0, 0x19866891)
    struct.pack_into('<II', head, 0x4c, 1, 0)
    struct.pack_into('<3I', head, 0x90, len(section), len(head), 0)
    struct.pack_into('<4I', head, 0x90 + 0x30, 0, len(control), len(code) + 0x20, len(control))
    out = bytes(head) + section
    struct.pack_into('<I', head, 0x44, len(out))
    return bytes(head) + section, hashlib.sha256(bytes(control[:key_len]) + code).hexdigest()


def idrk(data, chunk=0x4000):
    """An IDRK entry holding data as zlib in chunks (flag 0x100000), as KTGL's RDB data files keep their containers."""
    stored = b''.join(struct.pack('<I', len(z)) + z for z in
                      (zlib.compress(data[i:i + chunk]) for i in range(0, len(data), chunk))) + bytes(4)
    head = bytearray(0x40)
    head[:8] = b'IDRK0000'
    struct.pack_into('<QQQ', head, 8, len(head) + len(stored), len(stored), len(data))
    struct.pack_into('<I', head, 0x2c, 0x100000)
    return bytes(head) + stored


class KtglShaders(unittest.TestCase):
    def test_glslc_outputs_keyed_as_the_runtime(self):
        out, key = glslc_output(1, b'\x05' * 0x80)
        found = list(ktgl_shaders.shaders_in(bytes(16) + out + bytes(8)))
        self.assertEqual([(k, st) for k, st, _, _ in found], [(key, 'ps')])
        self.assertEqual(len(found[0][2]), 0x880, 'the control cut to what the runtime hashes')

    def test_short_control_sections_and_compute(self):
        out, key = glslc_output(0, b'\x06' * 0x40, key_len=0x800)
        self.assertEqual([k for k, *_ in ktgl_shaders.shaders_in(out)], [key])

    def test_not_a_shader(self):
        out, _ = glslc_output(3, b'\x07' * 0x40)   # a stage the translator does not take
        self.assertEqual(list(ktgl_shaders.shaders_in(out)), [])
        bad = bytearray(glslc_output(0, b'\x07' * 0x40)[0]); struct.pack_into('<I', bad, 0x90 + 0x90, 0)   # no control magic
        self.assertEqual(list(ktgl_shaders.shaders_in(bytes(bad))), [])

    def test_idrk_entries_and_chunks(self):
        out, key = glslc_output(0, b'\x08' * 0x40)
        payload = b'_S2G3000' + bytes(8) + out * 3 + os.urandom(0x9000)
        data = bytes(5) + idrk(payload) + idrk(b'other entry')
        entries = list(ktgl_shaders.idrk_entries(data))
        self.assertEqual(len(entries), 2)
        a, b, flags = entries[0]
        self.assertEqual(ktgl_shaders.entry_data(data, a, b, flags, 4), b'_S2G')
        self.assertEqual(ktgl_shaders.entry_data(data, a, b, flags), payload)
        with self.assertRaises(ktgl_shaders.Damaged):
            list(ktgl_shaders.idrk_entries(data[:-20]))

    def test_file_shaders_reads_only_shader_containers(self):
        vs, vs_key = glslc_output(0, b'\x09' * 0x40)
        ps, ps_key = glslc_output(1, b'\x0a' * 0x40)
        data = idrk(b'_DOK0000' + vs) + idrk(b'_TEX0000' + ps)
        with tempfile.TemporaryDirectory() as d:
            image = Path(d, 'romfs.bin'); image.write_bytes(bytes(32) + data)
            path, found, containers = ktgl_shaders.file_shaders((str(image), 'asset/system.rdb.bin', 32, len(data)))
            self.assertEqual(([k for k, *_ in found], containers), ([vs_key], 1))


class JumpTableThroughLr(unittest.TestCase):
    def test_the_index_register_x30_is_read(self):
        # capstone names x30 "lr": add x18, x18, x30, lsl #2 after ldrb w30, [...] is a byte table (main+0x13847dc)
        text = (HERE / 'third_party/d2r/patch_nso.py').read_text()
        self.assertIn("idx = {'lr': 30, 'fp': 29}.get(p.reg_name(p.operands[2].reg)) or num(p.reg_name(p.operands[2].reg))", text)
        if not rewriter_deps():
            self.skipTest('capstone')
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
        ins = next(md.disasm(struct.pack('<I', 0x8b1e0a52), 0))   # add x18, x18, x30, lsl #2
        self.assertEqual(ins.reg_name(ins.operands[2].reg), 'lr')


@unittest.skipUnless(rewriter_deps(), 'capstone and lz4 (the rewriter\'s modules)')
class LeadingPatchArea(unittest.TestCase):
    def rewrite(self, bss):
        tmp = Path(self.tmp.name)
        src, out = tmp / f'nso-{bss:x}', tmp / f'out-{bss:x}'
        src.write_bytes(synthetic_nso.build(bss))
        out.mkdir()
        subprocess.run([sys.executable, str(HERE / 'third_party/d2r/patch_nso.py'), str(src), str(out), 'main'], check=True,
                       capture_output=True, cwd=HERE / 'third_party/d2r', env={**os.environ, 'DM_RULESET': 'native', 'DM_PRIVATE_GATES': '0'})
        return out, fixups(out, 'main'), (out / 'main.patched.bin').read_bytes()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def test_large_bss_puts_the_patch_area_before_the_module(self):
        bss = 0xC000000   # 192 MiB: an after-image patch area would be out of B's 128 MiB reach
        _, f, data = self.rewrite(bss)
        lead = f['PATCH_LEAD']
        self.assertEqual(f['PATCH_OFF'], -lead)
        self.assertGreater(lead, 0)
        self.assertEqual(f['IMAGE_SIZE'], f['KEY_PAGE_OFF'] + 0x4000)
        self.assertGreaterEqual(f['KEY_PAGE_OFF'], synthetic_nso.DATA + 0x1000 + bss)
        # the file: the patch area, then the module up to the end of data only (.bss and the key page are zero fill)
        self.assertEqual(len(data), lead + 0x4000 * -(-(synthetic_nso.DATA + 0x1000) // 0x4000))
        self.assertLessEqual(f['PATCH_SIZE'], lead)
        self.assertTrue(any(data[:f['PATCH_SIZE']]))
        self.assertFalse(any(data[f['PATCH_SIZE']:lead]))
        # the rewritten site branches back into the patch area, inside B's reach
        site = synthetic_nso.FUNC
        target = branch_target(struct.unpack_from('<I', data, lead + site)[0], site)
        self.assertTrue(-lead <= target < -lead + f['PATCH_SIZE'], hex(target))
        # the module's other bytes are the original ones
        original = synthetic_nso.build(bss)
        self.assertEqual(data[lead + synthetic_nso.RO:lead + synthetic_nso.RO + 0x1000], original[0x1100:0x2100])

    def test_small_bss_keeps_the_patch_area_after_the_image(self):
        _, f, data = self.rewrite(0x100000)
        self.assertEqual(f['PATCH_LEAD'], 0)
        self.assertGreater(f['PATCH_OFF'], f['KEY_PAGE_OFF'])
        self.assertEqual(len(data), f['PATCH_OFF'] + f['PATCH_SIZE'])
        site = synthetic_nso.FUNC
        target = branch_target(struct.unpack_from('<I', data, site)[0], site)
        self.assertTrue(f['PATCH_OFF'] <= target < f['PATCH_OFF'] + f['PATCH_SIZE'], hex(target))

    def test_prepare_runtime_and_loader_know_the_layout(self):
        prepare = (HERE / 'scripts/prepare_runtime.py').read_text()
        self.assertIn("image = data[layout['PATCH_LEAD']:]", prepare)
        self.assertIn('.patch_lead={l["PATCH_LEAD"]}ULL', prepare)
        loader = (HERE / 'runtime/loader.c').read_text()
        self.assertIn('m->base=reserved+m->patch_lead;', loader)
        self.assertIn('fseeko(f,(off_t)m->patch_lead,SEEK_SET)', loader)
        self.assertIn('int64_t patch_off; uint64_t patch_lead, patch_size, image_size;', (HERE / 'runtime/runtime.h').read_text())


class Graphics(unittest.TestCase):
    def test_a_compute_unbind_keeps_the_graphics_program(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('const prog_t *prog, *cprog;', src)
        self.assertIn('if (stages && !(stages & NVN_SHADER_STAGE_ALL_GRAPHICS_BITS)) return S.prog;', src)
        self.assertIn('if ((bp->stages & NVN_SHADER_STAGE_COMPUTE_BIT) && prog_has_compute(bp->prog)) S.cprog = bp->prog;', src)
        body = src[src.index('static void dispatch(const p_dispatch *dp) {'):src.index('static void dispatch_bound(const p_dispatch *dp) {\n    ')]
        self.assertLess(body.index('S.prog = S.cprog;'), body.index('dispatch_bound(dp);\n    S.prog = graphics;'))

    def test_a_buffer_copy_owns_its_texture(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        b2t = src[src.index('static void copy_b2t(const p_copy_b2t *c) {'):]
        b2t = b2t[:b2t.index('\n}\n')]
        self.assertIn('gpu_owned_mark(c->tex->serial);', b2t)
        self.assertLess(b2t.index('mtl_addr(c->src'), b2t.index('gpu_owned_mark('))

    def test_copy_image_stride(self):
        hle = (NVN / 'hle_nvn.c').read_text()
        self.assertIn('NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { *REC(c, OP_COPY_STRIDE, p_copy_stride) = (p_copy_stride){st, 1, 0}; }', hle)
        self.assertEqual((NVN / 'nvn_mtl.m').read_text().count('img = S.copy_image_stride ? (size_t)S.copy_image_stride : row * rows;'), 2)

    @unittest.skipUnless(sys.platform == 'darwin', 'Foundation')
    def test_c1_beyond_4_kb(self):
        probe = r"""
#import <Foundation/Foundation.h>
#include "nvn_refl.h"
int main(void) { @autoreleasepool {
    NSMutableString *hex = [NSMutableString string];
    for (int i = 0; i < 4352; i++) [hex appendFormat:@"%02x", i & 0xff];
    NSDictionary *j = @{@"route": @"B", @"bindings": @[], @"c1": hex, @"vertex_inputs": @[]};
    nvn_refl r; char err[256] = "";
    if (nvn_refl_from_json(j, &r, err, sizeof err)) { printf("fail %s\n", err); return 1; }
    for (int i = 0; i < 4352; i++) if (r.c1[i] != (i & 0xff)) { printf("byte %d\n", i); return 1; }
    printf("ok %u %d\n", r.c1_len, r.has_c1); free(r.c1);
    NSMutableString *big = [NSMutableString string]; for (int i = 0; i < 0x10000; i++) [big appendString:@"00"];
    nvn_refl q; if (!nvn_refl_from_json(@{@"route": @"B", @"bindings": @[], @"c1": big}, &q, err, sizeof err)) return 2;
    if (q.c1 || q.c1_len) return 3;
    return 0; } }
"""
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'p.m').write_text(probe)
            exe = Path(d) / 'p'
            build = subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-I', str(NVN), '-I', str(NVN / 'include'), str(Path(d) / 'p.m'),
                                    str(NVN / 'nvn_refl.m'), '-framework', 'Foundation', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            out = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual((out.returncode, out.stdout.strip()), (0, 'ok 4352 1'))
        self.assertIn('- (void)dealloc { free(rf.c1); }', (NVN / 'nvn_mtl.m').read_text())


class TranslatorAndBuffers(unittest.TestCase):
    """The first battle's GPU hang (2026-11-22): its compute shaders read their light lists through a combined texture
    handle (c[2] + 0x20) bound to a buffer texture. Odyssey's translator (the v4 chain of games/mk8dx and games/rdr) had no
    slot for such a handle, made the read a 2D one and recorded the slot as any type ("*"), so that translation was used
    with the buffer bound: undefined on Metal, loop counts read garbage, the GPU never finished."""

    def test_tears_of_the_kingdoms_translator(self):
        link = HERE / 'prebuilt/macos-arm64/totk_xlate'
        totk = ROOT / 'games/totk/prebuilt/macos-arm64'
        self.assertEqual(link.resolve(), (totk / 'totk_xlate').resolve(), 'the Zelda line\'s current translator')
        self.assertEqual((HERE / 'prebuilt/macos-arm64/totk_xlate.sha256').read_text(), (totk / 'totk_xlate.sha256').read_text())
        program = (ROOT / 'games/totk/tools/totk_xlate/Program.cs').read_text()
        self.assertIn('if (bytes >= CombinedBase && bytes < CombinedBase + 8 * CombinedSlots && (bytes - CombinedBase) % 8 == 0) return (bytes - CombinedBase) / 8;', program)

    def test_a_new_chain_and_its_users(self):
        chain = re.search(r'#define DMSH_CHAIN "([^"]+)"', (NVN / 'dmsh_chain.h').read_text()).group(1)
        self.assertEqual(chain, 'aocx-native-v6-shared-memory', 'no translation made without the kernels\' shared memory is reused')
        for f in ('ios/tools/build_ipa.py', 'scripts/prepare_aocx_shader_seed.py'):
            self.assertIn(f"CHAIN_CACHE = 'shader-cache/rt-{chain}'", (HERE / f).read_text(), f)
        seed = load(HERE / 'scripts/prepare_aocx_shader_seed.py', 'aocx_seed_chain')
        self.assertEqual(seed.runtime_chain(), chain)
        self.assertIn('seed.CHAIN = runtime_chain()', (HERE / 'scripts/prepare_aocx_shader_seed.py').read_text())

    def test_a_texture_buffer_never_sits_in_another_types_slot(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        guard = 'if (t && t.textureType == MTLTextureTypeTextureBuffer && b->dtt != MTLTextureTypeTextureBuffer) {'
        self.assertIn(guard, src)
        bind = src[src.index(guard):]
        self.assertLess(bind.index('t = nil; }'), bind.index('if (!t) { t = dummy_tex(b->dtt, b->dpf);'), 'the dummy instead')

    def test_an_integer_texture_buffer_is_read_with_the_declared_class(self):
        # the light grid (R32 / RG32 integer) read through texture_buffer<float>: undefined on Metal, the deferred-lighting
        # loop counts were garbage (the maintainer's run, fragment 3f17774d); a same-bits view of the declared class
        src = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('{MTLPixelFormatR32Float, MTLPixelFormatR32Uint, MTLPixelFormatR32Sint},', src)
        self.assertIn('{MTLPixelFormatRG32Float, MTLPixelFormatRG32Uint, MTLPixelFormatRG32Sint},', src)
        self.assertIn('{MTLPixelFormatRGBA32Float, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint}};', src)
        self.assertIn('v = [t.buffer newTextureWithDescriptor:d offset:t.bufferOffset bytesPerRow:t.bufferBytesPerRow];', src, 'the same memory')
        tbuf = src[src.index('} else if (b->kind == NVN_RB_TBUF) {'):]
        tbuf = tbuf[:tbuf.index('USE(t, MTLResourceUsageRead);')]
        self.assertIn('id<MTLTexture> cv = tbuf_class_view(t, b->dpf);', tbuf)
        self.assertLess(tbuf.index('tbuf_class_view'), tbuf.index('slots[aid] = trid;'))

    def test_compute_shared_memory_comes_from_the_control_section(self):
        # The root cause of the first battle's GPU hang (2026-11-22, the maintainer's Mac panicked five times): the
        # particle kernel dc81d690 keeps 68 bytes per thread from byte 304 for 64 threads (4656 bytes of shared memory,
        # control+0x754), but the translator was never told and declared its 4 KiB default; threads past it wrote outside
        # the threadgroup array, the counts its loops run to became garbage and the GPU never finished. Reproduced on every
        # unfixed run, gone on every run with the size passed (runs f1-f3).
        seed = load(HERE / 'scripts/prepare_aocx_shader_seed.py', 'aocx_seed_shared')
        with tempfile.TemporaryDirectory() as d:
            for raw, want in ((4656, 4656), (4650, 4656), (8192, 8192), (4096, None), (2048, None), (0, None), (0x10000, None)):
                control = bytearray(0x800)
                struct.pack_into('<II', control, 0x700, 0, 0x40)
                struct.pack_into('<III', control, 0x748, 64, 1, 1)
                struct.pack_into('<I', control, 0x754, raw)
                rq = seed.translation_request('v', 'cs', bytes(control), bytes(0x40), {}, Path(d) / 'code')
                self.assertEqual(rq.get('shared'), want, raw)
                self.assertEqual(rq['local'], [64, 1, 1])
            vs = seed.translation_request('v', 'vs', bytes(control), bytes(0x40), {}, Path(d) / 'code')
            self.assertNotIn('shared', vs, 'compute only')
        rt = (NVN / 'totk_shader_rt.m').read_text()
        self.assertIn('#include "compute_shared.h"', rt, 'the shared rule (tests/test_compute_shared_memory.py)')
        self.assertEqual((NVN / 'compute_shared.h').resolve(), (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/compute_shared.h').resolve())
        self.assertIn('if (stage == 5 && totk_rt_compute_shared_request(control)) rq[@"shared"] = @(totk_rt_compute_shared_request(control));', rt)
        program = (ROOT / 'games/totk/tools/totk_xlate/Program.cs').read_text()
        self.assertIn('public int QueryComputeSharedMemorySize() => _job["shared"] is JsonNode n ? (int)n : 0;', program, 'the translator reads it')
        service = (ROOT / 'games/totk/tools/totk_xlate/Service.cs').read_text()
        self.assertIn('if (Get(rq, "shared") is JsonValue sh) job["shared"] = sh.DeepClone();', service)

    def test_the_seed_and_the_host_cap_loops(self):
        self.assertIn('msl_loop_cap.cap_folder(output)', (HERE / 'scripts/prepare_aocx_shader_seed.py').read_text())
        self.assertIn('NSString *src = msl_loop_cap(msl);', (NVN / 'nvn_mtl.m').read_text())


class HostAnswers(unittest.TestCase):
    def test_sdk9_entry_point(self):
        loader = (HERE / 'runtime/loader.c').read_text()
        self.assertIn('required("_ZN2nn4init5StartEmmPFvvES2_"))(main_handle,0,loader_ready,module_inits);', loader)
        self.assertNotIn('S2_S2_', loader)

    def test_this_games_answers(self):
        applet = (HERE / 'runtime/applet_host.c').read_text()
        for name in ('_ZN2nn2oe30InitializeCopyrightFrameBufferEPvm', '_ZN2nn2oe17SetCopyrightImageEPKvmiiiiNS0_16WindowOriginModeE',
                     '_ZN2nn2oe22SetCopyrightVisibilityEb', '_ZN2nn3err9ShowErrorENS0_9ErrorCodeE'):
            self.assertIn(f'"{name}"', applet)
        self.assertIn('static void show_error_code(uint64_t code)', applet, 'ErrorCode comes by value in x0')
        self.assertIn('{"_ZN2nn3hid18GetNpadJoyHoldTypeEv",get_hold_type}', (HERE / 'runtime/input.c').read_text())
        services = (HERE / 'runtime/services.c').read_text()
        self.assertIn('void *movie=runtime_movie_override(name);if(movie)return movie;', services)
        self.assertIn('#define SYSTEM_RESOURCE_BUDGET 0ULL', services)
        self.assertIn('if (movie_host_tick) movie_host_tick();', (NVN / 'hle_nvn.c').read_text())

    def test_headless_layer(self):
        front = (HERE / 'runtime/frontend.m').read_text()
        self.assertIn('static void vi_initialize(void){headless_layer();', front)
        self.assertIn('if(metalLayer||!getenv("TOTK_HEADLESS"))return;', front)

    def test_a_headless_run_ends_after_the_gpu(self):
        # runs killed in the middle of GPU work left the GPU busy and its memory held (2026-11-22): "close", SIGTERM and
        # SIGINT end a headless run after the submitted work completes (mtl_quiesce), at most 10 s later
        front = (HERE / 'runtime/frontend.m').read_text()
        close = front[front.index('static void *headless_close(void *arg){'):front.index('static int headlessSignalPipe')]
        self.assertLess(close.index('int ok=mtl_quiesce(10000000000ull);'), close.index('_exit(ok?0:4);'))
        self.assertIn('signal(SIGTERM,headless_signal);signal(SIGINT,headless_signal);', front)
        self.assertIn('if(getenv("TOTK_HEADLESS")){pthread_t t;if(!pthread_create(&t,NULL,headless_close,NULL))', front)
        self.assertIn('int mtl_quiesce(uint64_t timeout_ns) { return mtl_wait(g_fence, timeout_ns); }', (NVN / 'nvn_mtl.m').read_text())

    def test_gpu_hang_guard(self):
        src = (NVN / 'nvn_mtl.m').read_text()
        wait = src[src.index('int mtl_wait(uint64_t fence, uint64_t timeout_ns) {'):]
        wait = wait[:wait.index('\n}\n')]
        self.assertIn('if (timeout_ns == UINT64_MAX && gpu_hang_s())', wait)
        self.assertIn('if (++still >= (uint64_t)gpu_hang_s()) gpu_hang_stop(fence, now);', wait)
        self.assertIn('v = e ? atoi(e) : 10;', src, 'on by default: 10 s')
        self.assertEqual(src.count('gpu_ring_note('), 3, 'draws and dispatches are noted for the report')

    def test_the_movies_link_libvpx(self):
        make = (HERE / 'Makefile').read_text()
        self.assertIn('$(VPX_PREFIX)/lib/libvpx.a', make)
        self.assertIn('-DGAME_AOCX=1', make)


def crypto_available():
    try:
        import cryptography  # noqa: F401
        return True
    except ImportError:
        return False


@unittest.skipUnless(crypto_available(), 'cryptography (the NCA readers)')
class AddOnContent(unittest.TestCase):
    def setUp(self):
        self.mod = load(HERE / 'scripts/extract_dlc.py', 'aocx_dlc')

    def test_pins_and_indexes(self):
        pins = json.loads((HERE / 'assets/expected-dlc.json').read_text())['addons']
        self.assertEqual(sorted(pins), ['1', '2', '3', '4'])
        for index, pin in pins.items():
            self.assertEqual(int(pin['title'], 16) - self.mod.AOC_BASE, int(index))

    def test_a_folder_of_nsps(self):
        with tempfile.TemporaryDirectory() as d:
            for n in ('b.nsp', 'a.NSP', 'note.txt'):
                Path(d, n).write_text('x')
            self.assertEqual([p.name for p in self.mod.addon_files([Path(d)])], ['a.NSP', 'b.nsp'])


class ZeldaLine(unittest.TestCase):
    def test_no_nxnative_code(self):
        self.assertFalse((HERE / 'third_party/nxnative').exists())
        for f in HERE.rglob('*'):
            if f.is_symlink():
                self.assertNotIn('nxnative', str(f.readlink()), f)
                self.assertNotIn('games/aoc/', str(f.resolve()), f)
        for f in (HERE / 'scripts').glob('*.py'):
            self.assertNotIn('games/aoc/', f.read_text(), f)

    def test_runtime_links_the_shared_files_rdr_links(self):
        rdr = ROOT / 'games/rdr/runtime'
        for f in rdr.iterdir():
            if f.is_symlink() and 'shared/runtime' in str(f.readlink()):
                mine = HERE / 'runtime' / f.name
                self.assertTrue(mine.is_symlink() and mine.resolve() == f.resolve(), f.name)
        for name in ('movie_host.c', 'movie_pace.h', 'audio_out.c'):
            self.assertEqual((HERE / 'runtime' / name).resolve(), (ROOT / 'shared/runtime' / name).resolve())
        self.assertIn('defined(GAME_AOCX)', (ROOT / 'shared/runtime/offline_host.c').read_text())

    def test_game_switch(self):
        self.assertIn('#define GAME_AOCX 1', (HERE / 'runtime/game_version.h').read_text())
        self.assertIn('mng_active_start(@"aocx"', (HERE / 'runtime/frontend.m').read_text())
        self.assertIn('org.macnativegames.aocx-native', (HERE / 'runtime/game_info.plist').read_text())
        pins = json.loads((HERE / 'assets/expected-1.4.1.json').read_text())
        self.assertEqual((pins['title_id'], pins['version_raw']), ('01002B00111A2000', 458752))


class SeedTool(unittest.TestCase):
    """scripts/prepare_aocx_shader_seed.py: Odyssey's seed tool (the Zelda line's) with this game's discovery, depth and
    rules; nothing of the tool copied."""

    def setUp(self):
        self.mod = load(HERE / 'scripts/prepare_aocx_shader_seed.py', 'aocx_seed')

    def test_runs_the_zelda_lines_tool(self):
        text = (HERE / 'scripts/prepare_aocx_shader_seed.py').read_text()
        self.assertIn("GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'", text, 'the tool is run, not copied')
        self.assertNotIn('def prepare(', text)
        with tempfile.TemporaryDirectory() as d:
            seed = self.mod.load_seed_tool(Path(d), Path(d) / 'rules.tsv')
            self.assertIs(seed.inventory, self.mod.inventory)
            self.assertIs(seed.translation_request, self.mod.translation_request)
            self.assertEqual(seed.CHAIN, 'aocx-native-v6-shared-memory', 'this runtime\'s chain, not Odyssey\'s')

    def test_depth_is_zero_to_one(self):
        control = bytearray(0x880); struct.pack_into('<II', control, 0x700, 0x30, 0x40)
        with tempfile.TemporaryDirectory() as d:
            request = self.mod.translation_request('k-v', 'vs', bytes(control), bytes(range(0x40)), {'attr': {}}, Path(d) / 'c')
            self.assertIs(request['depth_minus_w'], False)
            self.assertEqual((Path(d) / 'c').read_bytes(), bytes(range(0x30, 0x40)))
            out = Path(d) / 'seed'; out.mkdir()
            v = 'a' * 64 + '-' + 'b' * 12
            (out / f'{"a" * 64}.variants').write_text(json.dumps([{'variant': v, 'deps': {}}]))
            (out / f'{v}_b.json').write_text(json.dumps({'stage': 'vs'})); (out / f'{v}_b.metal').write_text('m')
            (out / 'shader-keys.json').write_text('{}')
            (out / 'seed-manifest.json').write_text(json.dumps({'chain': 'c', 'files': {}}))
            self.assertEqual(self.mod.finish(out, 'c'), 1)
            self.assertIs(json.loads((out / f'{"a" * 64}.variants').read_text())[0]['depth_minus_w'], False)
            self.assertFalse((out / 'shader-keys.json').exists(), 'the importer refuses it')
            self.assertEqual(set(json.loads((out / 'seed-manifest.json').read_text())['files']),
                             {f'{"a" * 64}.variants', f'{v}_b.json', f'{v}_b.metal'})

    def test_rules_are_the_macs_vertex_layouts(self):
        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            rows = {'vs': [{'variant': 'a-1', 'deps': {'attr': {'0': 'float', '1': 'uint', '2': 'float'}}},
                           {'variant': 'a-2', 'deps': {'attr': {}}}],
                    'ps': [{'variant': 'b-1', 'deps': {'attr': {'0': 'uint'}}}]}
            for i, (stage, rs) in enumerate(rows.items()):
                (cache / f'{i}.variants').write_text(json.dumps(rs))
                for r in rs:
                    (cache / f'{r["variant"]}_b.json').write_text(json.dumps({'stage': stage}))
            rules = self.mod.rules_from_cache(cache)
            self.assertEqual(rules, ['{"attr":{"0":"float","1":"uint","2":"float"}}'])
            listed = cache / 'rules.tsv'
            shaders = {'c' * 64: ('vs', b'', b'', 's'), 'd' * 64: ('ps', b'', b'', 's')}
            self.assertEqual(self.mod.write_rules(listed, rules, shaders), 1)
            sys.path.insert(0, str(ROOT / 'games/odyssey/third_party/totk-scripts'))
            from shader_list import ShaderList
            self.assertEqual(ShaderList.load(listed).variants, {('c' * 64, rules[0])}, 'rows the tool reads')

    def test_inventory_reads_the_rdb_files_and_main(self):
        vs, vs_key = glslc_output(0, b'\x01' * 0x40)
        ps, ps_key = glslc_output(1, b'\x02' * 0x40)
        with tempfile.TemporaryDirectory() as d:
            romfs, modules = Path(d, 'romfs'), Path(d, 'modules')
            romfs.mkdir(); modules.mkdir()
            data = idrk(b'_S2G3000' + bytes(8) + vs)
            (romfs / 'romfs.bin').write_bytes(bytes(16) + data)
            (romfs / 'index.json').write_text(json.dumps({'status': 'pass', 'files': [
                {'path': 'asset/system.rdb.bin', 'offset': 16, 'size': len(data)}]}))
            (modules / 'main.patched.bin').write_bytes(bytes(64) + ps + vs)
            self.mod.MIN_SHADERS = 2
            _, shaders, startup, sources = self.mod.inventory(romfs, modules)
            self.assertEqual({k: v[3] for k, v in shaders.items()}, {vs_key: 'asset/system.rdb.bin', ps_key: 'module:main'})
            self.assertEqual(startup, set(shaders), 'every shader gets its default variant')
            self.assertEqual(sources, [{'path': 'asset/system.rdb.bin', 'size': len(data)}])
            self.mod.MIN_SHADERS = 3
            with self.assertRaises(ValueError):
                self.mod.inventory(romfs, modules)


if __name__ == '__main__':
    unittest.main()
