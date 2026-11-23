"""Ocarina of Time in VR renders at the size picked in the VR page (user 2026-11-23: "不能换分辨率 bug", "16:9依然不对
王国是大概1.1:1 的"). The row only restarted the stream while the game kept its 16:9 canvas. The canvas now follows the row
(games/oot/runtime/game_patches_ns2.c runtime_oot_vr_canvas, zeldaVR/gfx/totk_vr.m), and in VR the window textures are tall
enough for the largest size: a canvas larger than them stops the picture ("出图裁切越界")."""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCHES = ROOT / 'games/oot/runtime/game_patches_ns2.c'
VR = ROOT / 'zeldaVR/gfx/totk_vr.m'
SETTINGS = ROOT / 'games/oot/runtime/settings_game.h'

HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint8_t *base; } Module;
static void runtime_fail(const char *fmt, ...) { printf("refused: %%s\n", fmt); exit(3); }
%s
int main(void) {
    static uint8_t code[0x1203000];
    Module m = { code };
    uint32_t w[2] = { 0xd2814008, 0xf2c0b408 };
    memcpy(code + 0x1202ee0, w, 8);
    patch_oot_window_textures(&m);
    memcpy(w, code + 0x1202ee0, 8);
    printf("%%u %%u\n", (w[0] >> 5) & 0xffff, (w[1] >> 5) & 0xffff);
    return 0;
}
'''


def function(source, name):
    """The last definition (totk_vr.m has one-line stubs for the other platforms first), up to its closing brace."""
    found = re.findall(r'^(?:static )?[^\n(]*\b%s\([^\n;]*\{[^\n]*\n.*?^\}\n' % re.escape(name), source, re.S | re.M)
    if not found:
        raise AssertionError('%s is not in the source' % name)
    return found[-1]


class WindowTextures(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp()
        c = Path(cls.tmp) / 'window.c'
        c.write_text(HARNESS % function(PATCHES.read_text(), 'patch_oot_window_textures'))
        cls.exe = str(Path(cls.tmp) / 'window')
        subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', str(c), '-o', cls.exe], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def size(self, **extra):
        env = {k: v for k, v in os.environ.items() if k not in ('TOTK_VR_CONTROL', 'OOT_FB_SIZE', 'TOTK_EXPORT_IOS')}
        env.update(extra)
        out = subprocess.run([self.exe], env=env, check=True, capture_output=True, text=True).stdout
        return tuple(int(x) for x in out.split())

    def test_flat_play_keeps_3840x2160(self):
        self.assertEqual(self.size(), (3840, 2160))

    def test_vr_has_room_for_every_size_of_the_row(self):
        width, height = self.size(TOTK_VR_CONTROL='control.txt')
        row = re.search(r'static const int sizes\[\]\[2\]=\{(.*?)\};', SETTINGS.read_text())
        sizes = re.findall(r'\{(\d+),(\d+)\}', row.group(1))
        self.assertEqual(sizes, [('1344', '1408'), ('1680', '1760'), ('1920', '2048'), ('2048', '2208')])
        for w, h in sizes:
            self.assertLessEqual(int(w), width)
            self.assertLessEqual(int(h), height)

    def test_an_explicit_size_still_decides(self):
        self.assertEqual(self.size(TOTK_VR_CONTROL='control.txt', OOT_FB_SIZE='2560x1440'), (2560, 1440))


class Wiring(unittest.TestCase):
    def test_the_row_and_the_first_view_set_the_canvas(self):
        source = VR.read_text()
        self.assertIn('runtime_oot_vr_canvas', function(source, 'headlook_fov'))
        self.assertIn('runtime_oot_vr_canvas', function(source, 'vr_set_resolution'))

    def test_the_flat_resolution_row_leaves_the_vr_canvas_alone(self):
        body = function(PATCHES.read_text(), 'runtime_oot_canvas_set')
        guard, store = body.find('atomic_load(&oot_vr_canvas)'), body.find('atomic_store(oot_output_cell')
        self.assertGreaterEqual(guard, 0)
        self.assertLess(guard, store)

    def test_the_row_changes_the_render_and_the_stream_stays(self):
        """1344x1408 as both render and stream was full of blocks on the Quest while the game's own picture was clean
        (run mosaic01): the row is the render size, the video keeps its own."""
        body = function(VR.read_text(), 'vr_set_resolution')
        oot = body[body.index('#elif defined(GAME_OOT)'):]
        oot = oot[:oot.index('#else')]
        self.assertIn('runtime_oot_vr_canvas', oot)
        self.assertNotIn('streamer_stop_locked', oot)

    def test_the_stream_is_1680x1760_in_hevc(self):
        self.assertIn('VR_OOT_STREAM_WIDTH=1680,VR_OOT_STREAM_HEIGHT=1760', (ROOT / 'zeldaVR/gfx/vr_resolution_policy.h').read_text())
        source = VR.read_text()
        self.assertIn('VR_OOT_STREAM_WIDTH', function(source, 'stream_eye_size'))
        self.assertIn('defined(GAME_OOT)', function(source, 'vr_stream_resolution'))
        start = function(source, 'streamer_start_locked')
        self.assertIn('stream_eye_size(&sw,&sh)', start)
        self.assertRegex(start, r'#if defined\(TOTK_GAME_V143\) \|\| defined\(GAME_OOT\)\n\s*setenv\("BASALT_CODEC","hevc",0\)')

    def test_the_vr_canvas_is_rendered_at_full_scale(self):
        body = function(PATCHES.read_text(), 'runtime_oot_vr_canvas')
        self.assertIn('runtime_oot_scale_set(1)', body)
        self.assertIn('oot_canvas_relayout()', body)


if __name__ == '__main__':
    unittest.main()
