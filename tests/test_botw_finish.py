"""Read from the sources: Breath of the Wild 1.9.0's anti-aliasing (off, the game's FXAA, FXAA Quality, SMAA) and sharpening
(gfx/nvn/botw_finish.h, the effects page's rows as 1.4.3's totk_finish.h). Built on 1.9.0 only; the game's FXAA is its
upscale draw bd7ffdd4, which our pass replaces (RGB only) unless MetalFX already does or the game's own FXAA is chosen;
SMAA / FXAA Quality smooth the picture at its own size first, so the UI drawn later is never smoothed; the sharpening runs on the canvas before the first draw after the scene that
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
SMAA = (NVN / 'totk_smaa.h').read_text()
FXQ = (NVN / 'totk_fxaa_quality.h').read_text()
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
            'if (!MFXUP_COMPOSE(fs) || !R.rt.n')]                     # the compose path (TotK; BotW on iOS, 1ae9bba7)
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
        self.assertLess(up.index('if (replaced || !ours) return 0;'), up.index('rp_end();'))   # MetalFX's or the game's FXAA: never ours
        # The game's own FXAA is the only method that keeps its draw: off, SMAA and FXAA Quality all replace it.
        self.assertIn('int ours = off || smaa || quality;', up)
        self.assertIn('int smaa = !off && atomic_load_explicit(&g_fin_smaa', up)
        self.assertIn('int quality = !off && !smaa && atomic_load_explicit(&g_fin_fxaa_quality', up)
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
        self.assertLess(up.index('if (!pct && (replaced || !ours)) return 0;'), up.index('attachment_texture('))
        before = branch(FIN, 'static void botw_fin_before(', '\n}\n')
        self.assertLess(before.index('if (!pct || !c) return;'), before.index('rp_end();'))
        self.assertLess(before.index("if (!strncmp(fs->key, BOTW_MFXUP_KEY, 65)) return;"), before.index('rp_end();'))
        self.assertIn('fin_pass(c, g_bfin.scratch, pct)', before)        # totk_finish.h's RCAS, then copied back

    def test_smaa_and_fxaa_quality_on_the_picture_then_our_pass(self):
        # Both run on the picture at its size into a scratch of its size and format; the canvas is written only by our
        # RGB-only pass, so its alpha, pending clear and upscale stay as with FXAA off (never all four channels at 1:1).
        up = branch(FIN, 'static int botw_fin_upscale(', '\n}\n')
        self.assertIn('smaa_encode(g_cb, src, aa, slot)', up)
        self.assertIn('fxaa_quality_encode(g_cb, src, aa, slot)', up)
        self.assertIn('if (!bfin_pass(smoothed ? aa : src, dst, R.clear_c[0], R.cc[0])) return 0;', up)
        self.assertNotIn('smaa_encode(g_cb, src, dst', up)
        prep = branch(FIN, 'static id<MTLTexture> bfin_aa_prepare(', '\n}\n')
        self.assertIn('texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height', prep)
        self.assertIn('smaa_prepare(src, t, slot) : fxaa_quality_prepare(src, t, slot)', prep)

    def test_a_failed_prepare_keeps_the_games_fxaa(self):
        # Everything is prepared before any encoder is closed or the draw dropped: a failure returns 0 (the game's draw).
        up = branch(FIN, 'static int botw_fin_upscale(', '\n}\n')
        fail = up.index('if ((smaa || quality) && !aa) {')
        self.assertLess(up.index('bfin_aa_prepare(src, smaa, slot)'), fail)
        self.assertLess(fail, up.index('rp_end();'))
        self.assertIn('return 0;\n    }\n    rp_end();', up)

    def test_the_aa_passes_are_built_on_190_on_the_mac(self):
        guard = '#if TARGET_OS_OSX && (!defined(GAME_BOTW) || defined(GAME_BOTW190))'
        self.assertIn(guard, SMAA)
        self.assertIn(guard, FXQ)

    def test_in_session_flags(self):
        for flag, call in (('aa_method', 'dm_gfx_aa_method_set(v);'), ('smaa_quality', 'dm_gfx_smaa_quality_set(v);'),
                           ('fxaa_quality', 'dm_gfx_fxaa_quality_set(v);')):
            line = next(l for l in MTL.splitlines() if f'if (!strcmp(name, "{flag}"))' in l)
            self.assertIn(call, line)

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
