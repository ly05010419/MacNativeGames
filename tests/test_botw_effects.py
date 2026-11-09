"""Read from the sources (PR #1602): Breath of the Wild 1.9.0's ambient occlusion switch (the AO patch's helper returns
"nothing occluded" while c1[3].w bit 0 is set, before any other use of its parameters; dm_gfx_fx_available opens it on
1.9.0 only), its shadows switch (P9: the comparison samplers only, an Off choice in the shadow row) and its effects page (runtime/botw_effects.h: included on 1.9.0 only, the rows in Tears of the
Kingdom 1.4.3's order, graphics-effects.txt written and read as on 1.4.3, Reset turning the switches back on).
Read, not run: the BotW build and the game run are in the story."""
from pathlib import Path
import re
import unittest

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'
NVN = TOTK / 'third_party/d2r_gfx/gfx/nvn'
SSAO = (NVN / 'totk_ssao.h').read_text()
FX = (NVN / 'totk_fx.h').read_text()
PANEL = (TOTK / 'runtime/settings_panel.h').read_text()
PAGE = (TOTK / 'runtime/botw_effects.h').read_text()


def helper(name):
    """The MSL text of one helper string (its C string literals joined)."""
    body = re.search(rf'static const char {name}\[\] =\n(.*?);\n', SSAO, re.S)[1]
    return ''.join(re.findall(r'^\s*"(.*)\\n"', body, re.M))


def branch(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


class AoSwitch(unittest.TestCase):
    def test_the_botw_helper_returns_unoccluded_first_when_bit_0_is_set(self):
        body = helper('k_ssao_helper_botw').split('static inline float totk_ssao_botw(')[1]
        switch = 'if (fmod(p.w, 2.0) >= 0.5) return 1.0;'
        self.assertEqual(body.count(switch), 1)
        after_nan = body.index('p = select(p, float4(0.0), isnan(p));') + len('p = select(p, float4(0.0), isnan(p));')
        self.assertEqual(body[after_nan:].lstrip().index(switch), 0)        # the first statement after the NaN guard
        self.assertLess(body.index(switch), body.index('if (all(p == float4(0.0))) return stock;'))
        self.assertIn('if (fmod(p.w, 2.0) >= 0.5)', helper('k_ssao_helper_totk'))   # TOTK's own, unchanged

    def test_available_on_190_ao_grass_and_shadows(self):
        avail = branch(FX, 'int dm_gfx_fx_available(int i) {', '\n}\n')
        botw190 = branch(avail, '#if defined(GAME_BOTW190)', '#elif')
        self.assertIn('return (i == FX_AO && ssao_on()) || i == FX_GRASS || i == FX_SHADOW;', botw190)
        self.assertIn('(void)i; return 0;', branch(avail, '#elif defined(GAME_BOTW)', '#else'))
        self.assertIn('{"ao", ', FX)
        self.assertIn('if (i == FX_AO) ssao_set_off(!on);', FX)              # the switch writes the helper's w

    def test_grass_is_its_own_fragment_vertex_pair_on_190(self):
        table = branch(FX, 'static const fx_effect k_fx[] = {', '\n};\n')
        botw190 = branch(table, '#if defined(GAME_BOTW190)', '#else')
        self.assertIn('{"grass", "草", "Grass", {"d0cead9357ae8c7b/9a3f53268b7c3433"}, {0}},', botw190)   # P8: the pair seen
        self.assertIn('{"grass", "草", "Grass", {"33099f8c133950f5"}, {0}},', branch(table, '#else', '#endif'))   # TOTK's unchanged
        self.assertEqual(table.count('{"grass", '), 2)
        self.assertIn('FX_GRASS = 2', FX)
        ids = re.findall(r'\{"(\w+)", "', table)
        self.assertEqual(ids[:3], ['ao', 'shadow', 'grass'])   # FX_GRASS = 2 names it, on both sides of the #if

    def test_shadows_off_is_the_samplers_alone_on_botw(self):
        body = branch(FX, 'if (i == FX_SHADOW) {', '\n    }\n')
        self.assertIn('#ifndef GAME_BOTW\n        ssao_set_shadow_off(!on);', body)   # TOTK's pre-pass bit, not BotW's
        self.assertIn('dm_gfx_shadow_samplers_off(!on);', body.split('#endif')[1])


class Page(unittest.TestCase):
    def test_included_on_190_only(self):
        self.assertEqual(PANEL.count('#include "botw_effects.h"'), 1)
        before = PANEL[:PANEL.index('#include "botw_effects.h"')]
        self.assertTrue(before.rstrip().endswith('#ifdef GAME_BOTW190'))
        self.assertIn('#elif defined(GAME_BOTW190)\nstatic void settings_effects_restore(void);', PANEL)
        reset = branch(PANEL, 'static void settings_effects_reset(void) {', '\n}\n')
        self.assertIn('settings_botw_effects_reset();', branch(reset, '#ifdef GAME_BOTW190', '#endif'))

    def test_rows_in_the_order_of_143(self):
        rows = branch(PAGE, 'settings_effect_rows(void) {', '\n}\n')
        titles = re.findall(r'settings_text\("[^"]*","([^"]+)"\)|(settings_botw_shadow_row|settings_botw_fov_row)\(\)', rows)
        self.assertEqual([a or b for a, b in titles][1:9],                 # [0] is "Not in this version"
                         ['settings_botw_fov_row', 'Model detail distance (LOD)', 'Render distance',
                          'settings_botw_shadow_row', 'FXAA anti-aliasing', 'MetalFX replaces it', 'Depth of field',
                          'Sharpening'])
        self.assertIn('if(!strcmp(dm_gfx_fx_id(i),"shadow"))continue;', rows)
        self.assertIn('if(!dm_gfx_fx_available(i)){[rows addObject:settings_fixed(t,none)];continue;}', rows)

    def test_the_file_is_143s(self):
        self.assertEqual(PAGE.count('runtime_setting_save("graphics-effects.txt",n?line:"none","effects");'), 1)
        self.assertIn('if(!strcmp(w,dm_gfx_fx_id(i)))dm_gfx_fx_set(i,0);', PAGE)
        reset = branch(PAGE, 'static void settings_botw_effects_reset(void) {', '\n}\n')
        self.assertIn('if(!dm_gfx_fx_on(i))dm_gfx_fx_set(i,1);', reset)
        self.assertIn('(const char *const[]){"graphics-effects.txt","graphics-fxaa.txt","graphics-sharpen.txt",NULL}', reset)
        self.assertIn('runtime_setting_remove(*f);', reset)

    def test_the_shadow_row_offers_off_where_the_switch_acts(self):
        row = branch(PANEL, 'static TOTKSettingRow *settings_botw_shadow_row(void) {', '\n}\n')
        self.assertIn('return settings_botw190_shadow_row(t);', branch(row, '#else', '#endif'))
        self.assertIn('Not in this version', branch(row, '#ifndef GAME_BOTW190', '#else'))   # 1.6.0 unchanged
        own = branch(PAGE, 'static TOTKSettingRow *settings_botw190_shadow_row(NSString *t) {', '\n}\n')
        plain, with_off = own.split('if(fx<0||!dm_gfx_fx_available(fx))')[1].split('\n return settings_row(', 1)
        self.assertIn('@[@0,@2048]', plain); self.assertNotIn('@-1', plain)   # without the switch: the sizes only
        self.assertIn('@[@-1,@0,@2048]', with_off)
        self.assertIn('^int{return dm_gfx_fx_on(fx)?runtime_shadow_res_saved():-1;}', with_off)
        self.assertIn('^(int v){dm_gfx_fx_set(fx,v>=0);if(v>=0)runtime_shadow_res_save(v);settings_effects_save();}', with_off)
        self.assertIn('"Off (no shadows, at once)"', with_off)

    def test_within_300_lines(self):
        self.assertLessEqual(len(PAGE.splitlines()), 300)


if __name__ == '__main__':
    unittest.main()
