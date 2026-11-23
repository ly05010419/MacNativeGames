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
