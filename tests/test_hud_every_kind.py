"""The corner panel shows every kind of shader and pipeline a run collects or builds (user 2026-11-16: "都要显示 不要有隐藏，
我讨厌隐藏"). Before, shader-library (S) and compute (C) rows turned the box yellow with no number of theirs, mesh (M),
tile-read (T) and border (B) rows were not counted, and the pipelines and libraries made in play from the archive (the
player's 386 ms frame: 12 pipelines + 6 libraries) and the HDR / host pipelines in no list were shown nowhere."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'
FRONTENDS = [ROOT / f'games/{g}/runtime/frontend.m' for g in ('oot', 'totk', 'odyssey')]
HUD_BOX = ROOT / 'shared/runtime/hud_box.h'   # the corner box every frontend includes


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
        box = HUD_BOX.read_text()
        self.assertIn('static const int order[]=HUD_ORDER;', box)
        self.assertIn('if(!hud_line_shown(i)||!hud_snapshot(i,t,sizeof t,&hl))continue;', box)   # the overlay choice decides
        for f in FRONTENDS:
            with self.subTest(frontend=f.parent.parent.name):
                text = f.read_text()
                self.assertIn('#include "hud_box.h"', text)
                self.assertNotIn('@implementation TOTKHudBox', text)   # no copy of its own
                self.assertEqual((f.parent / 'hud_box.h').resolve(), HUD_BOX.resolve())

    def test_fps_full_information_is_a_settings_choice(self):   # user 2026-11-16: "在设置里 新建一个 叫 fps 全信息"
        self.assertIn('HUD_MODE_FULL = 3', self.header)
        for game in ('oot', 'totk', 'odyssey'):
            with self.subTest(game=game):
                panel = (ROOT / f'games/{game}/runtime/settings_game.h').read_text()   # its rows and restore (the shared panel reads it)
                # user 2026-11-18: "不要显示 括号里的 内容， 然后 全部 倒序", then "FPS 全信息" "改名 FPS和帧调试信息， 放最后"
                self.assertIn('@[settings_on_off(0),settings_text("极简 FPS","Bare FPS"),settings_text("FPS 与游玩时间","FPS + playtime"),settings_text("全部","All"),\n'
                              '    settings_text("FPS 和帧调试信息","FPS and frame debug info")]', panel)
                self.assertIn('@[@(HUD_MODE_OFF),@(HUD_MODE_BARE),@(HUD_MODE_MINIMAL),@(HUD_MODE_ALL),@(HUD_MODE_FULL)]', panel)
                self.assertNotIn('（只有 FPS 和分辨率', panel)
                self.assertNotIn('FPS 全信息', panel)
                self.assertIn('v<=HUD_MODE_BARE)fps_overlay_set_hud_mode(v)', panel)   # kept between runs

    def test_bare_fps_is_a_settings_choice(self):
        """User 2026-11-18: "fps 添加新布局 设置里 添加 极简fps 只显示 fps和分辨率， 放大3倍（只有这个 放大3倍）"."""
        self.assertIn('HUD_MODE_BARE = 4', self.header)
        self.assertIn('enum { HUD_BARE_SCALE = 3 };', self.header)
        for game in ('oot', 'totk', 'odyssey', 'acnh', 'eow'):   # every game that links the shared corner box
            with self.subTest(game=game):
                panel = (ROOT / f'games/{game}/runtime/settings_game.h').read_text()
                self.assertIn('settings_text("极简 FPS","Bare FPS")', panel)
                self.assertIn('@[@(HUD_MODE_OFF),@(HUD_MODE_BARE),@(HUD_MODE_MINIMAL),@(HUD_MODE_ALL),@(HUD_MODE_FULL)]', panel)
        overlay = (HUD_BOX.parents[1] / 'third_party/d2r_gfx/gfx/nvn/fps_overlay.c').read_text()
        self.assertIn('case HUD_MODE_MINIMAL: case HUD_MODE_BARE: return line == HUD_FPS;', overlay)   # its one line
        self.assertIn('snprintf(text, n, "FPS %s%s  %s", fps, gen, ss);', overlay)                     # the rate and the output size only
        self.assertIn('if (fps_overlay_hud_mode() == HUD_MODE_BARE) s *= HUD_BARE_SCALE;', overlay)     # in the picture's corner
        box = HUD_BOX.read_text()
        self.assertIn('monospacedSystemFontOfSize:bare?10.5*HUD_BARE_SCALE:10.5', box)                  # this mode alone is larger
        for line in ('if(playtime&&!bare)', 'int bpct=bare?-1:', 'NSString *build=bare?nil:', 'if(!bare)[a appendAttributedString:[[NSAttributedString alloc] initWithString:[hud_settings_hint()'):
            self.assertIn(line, box)   # nothing under the FPS line

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
