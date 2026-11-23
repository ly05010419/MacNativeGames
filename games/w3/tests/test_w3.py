"""The Witcher 3: Wild Hunt on the Zelda-line runtime: the fixes its bring-up needed, checked without the game."""
import json
from pathlib import Path
import re
import struct
import unittest

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
NVN = HERE / 'third_party/d2r_gfx/gfx/nvn'


class Identity(unittest.TestCase):
    def test_version_and_pins(self):
        v = (HERE / 'runtime/game_version.h').read_text()
        for line in ('#define GAME_W3 1', '#define GAME_FOLDER "W3"', '#define TOTK_GAME_VERSION "4.04b"',
                     '#define GAME_TITLE_ID 0x0100E67012924000ULL', '#define GAME_INBOX_ID "w3"'):
            self.assertIn(line, v)
        e = json.loads((HERE / 'assets/expected-4.04b.json').read_text())
        self.assertEqual(sorted(e['rewritten_sha256']), ['main.patched.bin', 'sdk.patched.bin'])
        self.assertEqual(e['romfs_size'], 21420806032)
        self.assertIn("MODULES = ('main', 'sdk')", (HERE / 'scripts/prepare_runtime.py').read_text())
        self.assertIn('-DGAME_W3=1', (HERE / 'Makefile').read_text())


class Runtime(unittest.TestCase):
    def test_system_resource_from_the_npdm(self):
        # 4 MB: nn::os enables virtual address memory, which a constructor of main uses at once
        self.assertIn('#define SYSTEM_RESOURCE_BUDGET (4ULL<<20)', (HERE / 'runtime/services.c').read_text())

    def test_the_answers_it_needed(self):
        src = ''.join((HERE / 'runtime' / f).read_text() for f in ('fs_host.c', 'input.c', 'services.c', 'system_services.c', 'time_host.c'))
        for name in ('_ZN2nn2fs25EnableGlobalFileDataCacheEPvm', '_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE',
                     '_ZN2nn3hid17InitializeGestureEv', '_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi',
                     '_ZN2nn3aoc28CheckAddOnContentMountStatusEv', '_ZN2nn4time23StandardUserSystemClock3nowEv',
                     '_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE'):
            self.assertIn(name, src)
        self.assertIn('#if defined(GAME_MK8DX) || defined(GAME_W3)', (ROOT / 'shared/runtime/account.c').read_text())
        self.assertIn('NVN_NOPROTO(nvnQueueBuilderSetQueuePriority', (NVN / 'hle_nvn.c').read_text())

    def test_shared_audio_out_ignores_data_offset(self):
        self.assertEqual((HERE / 'runtime/audio_out.c').resolve(), (ROOT / 'shared/runtime/audio_out.c').resolve())
        src = (ROOT / 'shared/runtime/audio_out.c').read_text()
        self.assertIn('need(d.data_size <= d.capacity || !d.capacity, "AudioOut buffer bounds");', src)
        self.assertIn('to_stereo(ipc_guest(d.sample_buffer, d.data_size)', src)

    def test_control_section_own_size(self):
        src = (NVN / 'hle_nvn.c').read_text()
        self.assertIn('if (len >= 0x7cc && len < CONTROL_BYTES) { memcpy(own, control, len); memset(own + len, 0, CONTROL_BYTES - len); control = own; }', src)

    def test_crash_waits_for_the_gpu(self):
        src = (HERE / 'runtime/loader.c').read_text()
        crash = src[src.index('static void crash('):]
        crash = crash[:crash.index('\n}\n')]
        self.assertLess(crash.index('mtl_quiesce(10000000000ull)'), crash.index('_exit(128+num);'))


class Textures(unittest.TestCase):
    def test_render_targets_are_writable(self):
        # the game writes render targets as images without the IMAGE flag; without ShaderWrite the scene stayed black
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('| ((t->d.target == 1 || t->d.target == 2 || t->d.target == 4) && !fmt_compressed(t->d.format) && !fmt_depth(t->d.format) && !packaged ? MTLTextureUsageShaderWrite : 0);', mtl)


class Stages(unittest.TestCase):
    def test_tessellation_runs_as_compute(self):
        # Metal 4 render encoders have no tessellator: capture, control kernel, tessellator, evaluation as a vertex function
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('if(st==3||st==4){o->tess=1;', mtl)
        self.assertIn('if (pr && pr->tess && !g_tess_ras.active) { tess_draw(pr, dw, di); return; }', mtl)
        self.assertIn('if (g_tess_ras.active) enc_r_draw_indirect(en, mp, g_tess_ras.args);', mtl)
        self.assertIn('NVN(nvnCommandBufferSetPatchSize, void, NVNcommandBuffer *c, int n) { REC(c, OP_PATCH_SIZE, p_flag)->value = n; }', (NVN / 'hle_nvn.c').read_text())
        self.assertNotIn('"nvnCommandBufferSetPatchSize",', (NVN / 'hle_nvn.c').read_text())
        self.assertIn('case RS_INDIRECT: [e drawPrimitives:(MTLPrimitiveType)u[0] indirectBuffer:u[1]]; break;', (NVN / 'nvn_render_stream.h').read_text())
        self.assertIn('static int g_red_once[256];', mtl)
        # a tessellation evaluation function feeds the rasterizer: its varyings are main0_out, its resources NVN stage 4
        self.assertIn('NSString *st = sh->stage == 0 || sh->stage == 4 ? @"main0_out" : @"main0_in";', mtl)
        self.assertIn('if (pr->sh[0]) encode_args(en, pr->sh[0], g_tess_ras.active ? 4 : 0, 1);', mtl)
        rt = (NVN / 'totk_shader_rt.m').read_text()
        self.assertIn('stage == 3 ? @"tcs" : stage == 4 ? @"tes"', rt)

    def test_capture_block_layout(self):
        # Metal aligns dm_geometry_capture's uint4 to 16 bytes; at 8 the capture wrote one record (2026-11-23 probe)
        inc = (NVN / 'nvn_tess_rt.inc').read_text()
        self.assertIn('typedef struct { uint64_t dst, pad; uint32_t info[4]; uint32_t stride; int16_t slot[TESS_REC_WORDS]; } tess_capture;', inc)
        self.assertIn('__builtin_offsetof(tess_capture, info) == 16', inc)

    def test_tessellator_partitioning(self):
        # the levels each spacing rounds to, and the triangles of a quad patch (the kernel's own formula, re-derived here)
        import math
        def level(f, spacing, outer):
            if outer and not f > 0: return 0
            if not f > 0: f = 1.0
            if spacing == 1: n = math.ceil(min(max(f, 2), 64)); return min(n + (n & 1), 64)
            if spacing == 2: return math.ceil(min(max(f, 1), 63)) | 1
            return math.ceil(min(max(f, 1), 64))
        self.assertEqual([level(x, 1, True) for x in (0, 0.5, 2, 2.1, 15.2, 16, 70)], [0, 2, 2, 4, 16, 16, 64])
        self.assertEqual([level(x, 2, True) for x in (1, 1.5, 4, 63.5)], [1, 3, 5, 63])
        def tris(o, nu, nv):
            if nu == 1 and nv == 1: return 2
            return 2 * (nu - 2) * (nv - 2) + sum(o[e] + (nu if e & 1 else nv) - 2 for e in range(4))
        # a uniform level n tiles the square into 2 n^2 triangles: interior plus the four zipped rings
        for n in (2, 4, 16, 64): self.assertEqual(tris([n] * 4, n, n), 2 * n * n)
        self.assertEqual(tris([1] * 4, 1, 1), 2)
        inc = (NVN / 'nvn_tess_rt.inc').read_text()
        self.assertIn('"    uint t = 2 * (nu - 2) * (nv - 2);\\n"', inc)


class Shaders(unittest.TestCase):
    def test_totk_translator_and_chain(self):
        link = HERE / 'prebuilt/macos-arm64/totk_xlate'
        self.assertEqual(link.resolve(), (ROOT / 'games/totk/prebuilt/macos-arm64/totk_xlate').resolve())
        self.assertEqual((HERE / 'prebuilt/macos-arm64/totk_xlate.sha256').read_text(),
                         (ROOT / 'games/totk/prebuilt/macos-arm64/totk_xlate.sha256').read_text())
        self.assertIn('#define DMSH_CHAIN "w3-native-v1"', (NVN / 'dmsh_chain.h').read_text())

    def test_packed_bank_names(self):
        src = (NVN / 'nvn_refl.m').read_text()
        self.assertIn('unsigned long tb = slots & 0xffff, sb = (slots >> 16) - 1, ty = h >> 28;', src)
        self.assertIn('if (reg < 0 && (r->kind == NVN_RB_TEX || r->kind == NVN_RB_SAMPT || r->kind == NVN_RB_SAMP)) {', src)
        # the example: fp_t_cb720898_2000405CSmplr = sampler handle in c10 word 1
        slots, h = 720898, 0x2000405C
        self.assertEqual(((slots >> 16) - 1, (h >> 14) & 0x3fff, h >> 28), (10, 1, 2))

    def test_driver_reads_per_word(self):
        service = (ROOT / 'games/totk/tools/totk_xlate/Service.cs').read_text()
        self.assertIn('["word_offsets"] = new JsonArray(words.Select(o => (JsonNode)o).ToArray())', service)
        self.assertIn('foreach (JsonNode o in (d["word_offsets"] ?? d["vec4_offsets"]).AsArray())', service)
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('if (!drv_fillable(o->stage, bank, w)) unfilled = 1;', mtl)
        self.assertIn('(off >= 0x610 && off < 0x620)', mtl)


class Rewriter(unittest.TestCase):
    def test_dispatch_forms(self):
        src = (HERE / 'third_party/d2r/patch_nso.py').read_text()
        self.assertIn("while k >= 0 and ctx[k].mnemonic in ('mov', 'add', 'sub', 'orr', 'and', 'eor') and not writes(ctx[k], 18)", src)
        self.assertIn('ctx = insl[max(0, j - 96):j]', src)


if __name__ == '__main__':
    unittest.main()
