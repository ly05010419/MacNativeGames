"""Read from the sources: Breath of the Wild 1.9.0's FXAA off and sharpening (gfx/nvn/botw_finish.h, the effects page's
rows as 1.4.3's totk_finish.h). Built on 1.9.0 only; the game's FXAA is its upscale draw bd7ffdd4, which our pass replaces
(RGB only) unless MetalFX already does; the sharpening runs on the canvas before the first draw after the scene that
writes or reads it, so before the UI and before key 2's scene copy; 0 % encodes nothing.
Read, not run: the builds and the game run are in the story."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
NVN = TOTK / 'third_party/d2r_gfx/gfx/nvn'
FIN = (NVN / 'botw_finish.h').read_text()
MTL = (NVN / 'nvn_mtl.m').read_text()
PAGE = (TOTK / 'runtime/botw_effects.h').read_text()


def branch(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


class Hooks(unittest.TestCase):
    def test_included_after_totk_finish_on_190_only(self):
        self.assertEqual(MTL.count('#include "botw_finish.h"'), 1)
        self.assertIn('#include "totk_finish.h"', MTL[:MTL.index('#include "botw_finish.h"')])
        guard = MTL[:MTL.index('#include "botw_finish.h"')].rstrip().splitlines()
        self.assertEqual(guard[-1], '#define BOTW_FINISH 1')   # Mac only: iOS has no FXAA / sharpening rows
        self.assertTrue(guard[-2].startswith('#if defined(GAME_BOTW190) && !TARGET_OS_IPHONE'))

    def test_every_call_is_on_190_only(self):
        for call in ('botw_fin_before(fs);', 'return botw_fin_upscale(fs, 0);', 'botw_fin_upscale(fs, 1);',
                     'if (botw_fin_upscale(fs, 0)) return 1;'):
            self.assertEqual(MTL.count(call), 1, call)
            before = MTL[:MTL.index(call)]
            self.assertGreater(before.rindex('#ifdef BOTW_FINISH'), before.rindex('#endif'), call)

    def test_the_order_in_mfxup_draw(self):
        draw = branch(MTL, 'static int mfxup_draw(NvProg *pr) {', '\n}\n')
        order = [draw.index(s) for s in (
            'if (fin_draw(fs)) return 1;',
            'botw_fin_before(fs);',                                     # sharpen first, for any draw after the scene
            'if (!mfxup_enabled()) return botw_fin_upscale(fs, 0);',     # MetalFX off: FXAA off acts
            'if (mfxup_dock(fs)) {',
            'botw_fin_upscale(fs, 1);',                                 # MetalFX wrote the canvas: only notes it
            'if (botw_fin_upscale(fs, 0)) return 1;',                    # MetalFX on but the draw kept
            'strncmp(fs->key, "0ac9a54938974396", 16)')]
        self.assertEqual(order, sorted(order))
        self.assertIn('    if (!mfxup_enabled()) return 0;\n#endif', draw)   # the other games unchanged

    def test_before_the_scene_copy_and_the_ui(self):
        call = MTL.index('if (mfxup_draw(pr)) return;')
        self.assertLess(call, MTL.index('mfx_snapshot_scene();   // TOTK_MFX: first UI draw into the canvas'))
        self.assertLess(call, MTL.index('BOTW_MFX_NO_UI_SCENE()   // key 4'))


class Passes(unittest.TestCase):
    def test_the_fxaa_draw_by_its_key(self):
        up = branch(FIN, 'static int botw_fin_upscale(', '\n}\n')
        self.assertIn('if (strncmp(fs->key, BOTW_MFXUP_KEY, 65)) return 0;', up)
        self.assertLess(up.index('if (replaced || !off) return 0;'), up.index('rp_end();'))   # MetalFX's or FXAA on: never ours
        for need in ('!(S.color.blend_en & 1)', 'bfin_mask_rgb_only(S.chmask.mask)', 'scissor_full(R.rt.color[0], cv, hv)',
                     'R.rt.n == 1'):
            self.assertIn(need, up)
        self.assertIn('R.clear_c[0] = 0;', up)                          # a pending clear is done by the pass

    def test_only_a_draw_writing_rgb_and_not_alpha_is_replaced(self):
        # Our pass writes RGB only: a game draw that also writes alpha (0xf) must keep the game's own. Compiled and run.
        fn = re.search(r'^static inline int bfin_mask_rgb_only\(uint32_t mask\) \{.*\}$', FIN, re.M)[0]
        d = tempfile.mkdtemp(); self.addCleanup(shutil.rmtree, d, True)
        cases = {0x7: 1, 0xf: 0, 0x3: 0, 0x0: 0, 0xe: 0, 0x17: 1, 0xf7: 1, 0x8f: 0}   # bits 4+ are other targets
        src = Path(d, 'mask.c')
        src.write_text('#include <stdint.h>\n#include <stdio.h>\n' + fn + '\nint main(void){' +
                       ''.join(f'printf("%d\\n",bfin_mask_rgb_only({m}u));' for m in cases) + 'return 0;}\n')
        subprocess.run(['xcrun', 'clang', '-Wall', '-Werror', str(src), '-o', str(Path(d, 'mask'))], check=True)
        out = subprocess.run([str(Path(d, 'mask'))], capture_output=True, text=True, check=True).stdout.split()
        self.assertEqual([int(v) for v in out], list(cases.values()))

    def test_rgb_only_and_texel_for_texel_at_one_to_one(self):
        self.assertIn('writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;', FIN)
        self.assertNotIn('MTLColorWriteMaskAlpha', FIN)
        self.assertIn('int scale = src.width != dst.width || src.height != dst.height;', FIN)
        self.assertIn('return t.read(uint2(in.p.xy));', FIN)
        self.assertIn('rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;', FIN)

    def test_zero_percent_encodes_nothing(self):
        up = branch(FIN, 'static int botw_fin_upscale(', '\n}\n')
        self.assertLess(up.index('if (!pct && (replaced || !off)) return 0;'), up.index('attachment_texture('))
        before = branch(FIN, 'static void botw_fin_before(', '\n}\n')
        self.assertLess(before.index('if (!pct || !c) return;'), before.index('rp_end();'))
        self.assertLess(before.index("if (!strncmp(fs->key, BOTW_MFXUP_KEY, 65)) return;"), before.index('rp_end();'))
        self.assertIn('fin_pass(c, g_bfin.scratch, pct)', before)        # totk_finish.h's RCAS, then copied back

    def test_within_300_lines(self):
        self.assertLessEqual(len(FIN.splitlines()), 300)
        self.assertLessEqual(len(Path(__file__).read_text().splitlines()), 300)


class Page(unittest.TestCase):
    def test_restore_and_reset(self):
        restore = branch(PAGE, 'static void settings_effects_restore(void) {', '\n}\n')
        self.assertIn('!strncmp(word,"off",3))dm_gfx_fxaa_set(0);', restore)
        self.assertIn('settings_read_number("graphics-sharpen.txt",0,100,&pct))dm_gfx_sharpen_set((int)pct);', restore)
        reset = branch(PAGE, 'static void settings_botw_effects_reset(void) {', '\n}\n')
        self.assertIn('dm_gfx_fxaa_set(1);dm_gfx_sharpen_set(0);', reset)

    def test_rows(self):
        rows = branch(PAGE, 'settings_effect_rows(void) {', '\n}\n')
        self.assertIn('dm_gfx_botw_fxaa_replaced()?settings_fixed(fxaa,', rows)
        self.assertIn('runtime_setting_save("graphics-fxaa.txt",v?"on":"off","FXAA")', rows)
        self.assertIn('settings_save_int("graphics-sharpen.txt",v,"sharpening")', rows)
        self.assertRegex(rows, re.escape('@[@0,@25,@50,@75,@100]'))


if __name__ == '__main__':
    unittest.main()
