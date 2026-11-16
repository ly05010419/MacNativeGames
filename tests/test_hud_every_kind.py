"""The corner panel shows every kind of shader and pipeline a run collects or builds (user 2026-11-16: "都要显示 不要有隐藏，
我讨厌隐藏"). Before, shader-library (S) and compute (C) rows turned the box yellow with no number of theirs, mesh (M),
tile-read (T) and border (B) rows were not counted, and the pipelines and libraries made in play from the archive (the
player's 386 ms frame: 12 pipelines + 6 libraries) and the HDR / host pipelines in no list were shown nowhere."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
FRONTENDS = [ROOT / 'games/oot/runtime/frontend.m', ROOT / 'games/totk/runtime/frontend.m']


class HudEveryKind(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.header = (NVN / 'fps_overlay.h').read_text()
        cls.mtl = (NVN / 'nvn_mtl.m').read_text()
        cls.mesh = (NVN / 'nvn_mesh_tasks.inc').read_text()

    def lines(self):
        enum = re.search(r'enum \{ (HUD_FPS[^}]*) \};', self.header).group(1)
        return [x.strip() for x in enum.split(',') if x.strip() != 'HUD_LINES']

    def test_the_order_has_every_line_once(self):
        order = re.search(r'#define HUD_ORDER \{([^}]*)\}', self.header).group(1).replace('\\\n', ' ')   # continued lines
        self.assertEqual(sorted(x.strip() for x in order.split(',')), sorted(self.lines()))

    def test_the_frontends_draw_every_line(self):
        for f in FRONTENDS:
            with self.subTest(frontend=f.parent.parent.name):
                text = f.read_text()
                self.assertIn('static const int order[]=HUD_ORDER;', text)
                self.assertNotIn('for(int i=0;i<4;i++){char t[128];int hl=0;', text)
                self.assertIn('if(!hud_line_shown(i)||!hud_snapshot(i,t,sizeof t,&hl))continue;', text)   # the overlay choice decides

    def test_fps_full_information_is_a_settings_choice(self):   # user 2026-11-16: "在设置里 新建一个 叫 fps 全信息"
        self.assertIn('HUD_MODE_FULL = 3', self.header)
        for game in ('oot', 'totk'):
            with self.subTest(game=game):
                panel = (ROOT / f'games/{game}/runtime/settings_panel.h').read_text()
                self.assertIn('settings_text("FPS 全信息', panel)
                self.assertIn('@[@(HUD_MODE_ALL),@(HUD_MODE_FULL),@(HUD_MODE_MINIMAL),@(HUD_MODE_OFF)]', panel)
                self.assertIn('v<=HUD_MODE_FULL)fps_overlay_set_hud_mode(v)', panel)   # kept between runs

    def test_every_line_but_fps_and_load_is_published_on_its_own(self):
        start = self.mtl.index('if (hud_compact()) {   // the window\'s corner panel')
        branch = self.mtl[start:self.mtl.index('return 0;', start)]
        self.assertIn('hitch_overlay_publish(now);', branch)   # the hitch lines (fps_overlay.c publishes them)
        for line in self.lines():
            if line in ('HUD_FPS', 'HUD_LOAD') or line.startswith('HUD_HITCH'):
                continue   # published by their own prepare functions / hitch_overlay_publish
            with self.subTest(line=line):
                self.assertIn(f'hud_line_publish({line},', branch)
        self.assertNotIn('hud_publish(', branch)   # one yellow per line, from its own text (hud_line_publish)

    def test_every_recorded_kind_is_counted_and_shown(self):
        recorded = set(re.findall(r'pso_user_record\("([A-Z])\\t', self.mtl + self.mesh))
        self.assertEqual(recorded, set('SRCMTB'))
        switch = self.mtl[self.mtl.index('static int collect_kind(char c)'):]
        switch = switch[:switch.index('\n}')]
        start = self.mtl.index('if (hud_compact()) {   // the window\'s corner panel')
        branch = self.mtl[start:self.mtl.index('return 0;', start)]
        shown = set(re.findall(r'g_collect_new\[COLLECT_([A-Z])\]', branch))
        prepare = self.mtl[self.mtl.index('static int shader_overlay_prepare('):start]
        shown |= set(re.findall(r'g_collect_new\[COLLECT_([A-Z])\]', prepare)) & {'R'}   # the render line's count (pso_new)
        for kind in sorted(recorded):
            with self.subTest(kind=kind):
                self.assertIn(f"case '{kind}': return COLLECT_{kind};", switch)
                self.assertIn(kind, shown)

    def test_every_long_frame_reaches_the_panel(self):   # user 2026-11-16: "我要对 每个卡顿 都有 可视化的 了解 不要隐藏"
        hle = (NVN / 'hle_nvn.c').read_text()
        judge = hle[hle.index('int lf = r->fr > 20.0 && r->fr > 1.4 * ef'):]
        note, log = judge.index('hitch_overlay_note(&h,'), judge.index('if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg))')
        self.assertLess(note, log)   # noted for the panel whether or not DM_LONGFRAME logs it
        self.assertIn('(lf || lg) && hitch_overlay_note', judge[:note])
        publish = (NVN / 'fps_overlay.c').read_text()
        publish = publish[publish.index('void hitch_overlay_publish('):]
        self.assertIn('HUD_HITCHES', publish)
        self.assertIn('HUD_HITCH0 + (int)i', publish)
        self.assertIn('enum { HITCH_KEEP = 3 };', self.header)

    def test_in_play_counts_start_at_the_title(self):
        self.assertLess(self.mtl.index('play_base_take();   // the box\'s "in play" counts start here'), self.mtl.index('if (runtime_warm_title) runtime_warm_title();'))
        self.assertIn('warm_light(); play_base_take();', self.mtl)   # TOTK_WARM=off has a title too
        take = self.mtl[self.mtl.index('static void play_base_take(void) {'):]
        take = take[:take.index('\n}')]
        for counter in ('g_par_hits', 'g_par_misses', 'g_rt_prebuilt', 'g_rt_compiled'):
            self.assertIn(f'atomic_load(&{counter})', take)
        hdr = self.mtl[self.mtl.index('if (k->a2c & (PSO_HDR | PSO_HOST_BITS)) {'):]
        self.assertLess(hdr.index('atomic_fetch_add(&g_pso_unlisted, 1)'), hdr.index('return;'))


if __name__ == '__main__':
    unittest.main()
