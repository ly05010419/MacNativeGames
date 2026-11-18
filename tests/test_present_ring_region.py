"""A present-ring slot holds the picture the display shows, not the game's whole window image.

Ocarina of Time's window images are 3840x2160 since the 4K canvas (patch_oot_window_textures) while a 1080p canvas shows
only the crop 0,0 1920x1080; the ring copied all 3840x2160 every frame (0.36-0.45 ms of GPU time, four 33 MB slots,
2026-11-18). The slot is now the crop alone and the present reads it from 0,0. An invalid crop keeps the whole image and
its own crop, so the present still refuses it by name; a source swapped after the copy (HDR) keeps its own crop too."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'

PROBE = r'''
#include <assert.h>
#include <stdio.h>
#include "nvn_present_ring_region.h"
static void same(nvn_ring_region r, size_t x, size_t y, size_t w, size_t h, int picture) {
    assert(r.x == x && r.y == y && r.w == w && r.h == h && r.picture == picture);
}
int main(void) {
    const int canvas[4] = {0, 0, 1920, 1080}, whole[4] = {0, 0, 3840, 2160}, inner[4] = {100, 50, 1280, 720};
    const int outside[4] = {0, 0, 3840, 2161}, shifted[4] = {2000, 1100, 1920, 1080}, none[4] = {0, 0, 0, 0}, negative[4] = {-1, 0, 100, 100};
    same(nvn_ring_region_for(3840, 2160, canvas), 0, 0, 1920, 1080, 1);     /* the 1080p canvas in the 4K window image */
    same(nvn_ring_region_for(3840, 2160, inner), 100, 50, 1280, 720, 1);
    same(nvn_ring_region_for(3840, 2160, NULL), 0, 0, 3840, 2160, 0);       /* no crop: the image */
    same(nvn_ring_region_for(3840, 2160, whole), 0, 0, 3840, 2160, 0);      /* the crop is the image */
    same(nvn_ring_region_for(3840, 2160, none), 0, 0, 3840, 2160, 0);       /* the present reads 0x0 as no crop */
    same(nvn_ring_region_for(3840, 2160, outside), 0, 0, 3840, 2160, 0);    /* invalid: the present refuses it itself */
    same(nvn_ring_region_for(3840, 2160, shifted), 0, 0, 3840, 2160, 0);
    same(nvn_ring_region_for(3840, 2160, negative), 0, 0, 3840, 2160, 0);
    same(nvn_ring_region_for(1280, 720, (const int[4]){0, 0, 1280, 720}), 0, 0, 1280, 720, 0);

    int out[4] = {9, 9, 9, 9};
    const int *c = nvn_ring_present_crop(1, 1920, 1080, canvas, out);        /* the slot is the picture: from 0,0 */
    assert(c == out && out[0] == 0 && out[1] == 0 && out[2] == 1920 && out[3] == 1080);
    c = nvn_ring_present_crop(1, 1280, 720, inner, out);
    assert(c == out && out[0] == 0 && out[1] == 0 && out[2] == 1280 && out[3] == 720);
    assert(nvn_ring_present_crop(0, 3840, 2160, outside, out) == outside);   /* the whole image: the game's own crop */
    assert(nvn_ring_present_crop(0, 3840, 2160, NULL, out) == NULL);
    puts("present ring region: ok");
    return 0;
}
'''


def function_body(src, signature):
    start = src.index(signature)
    return src[start:src.index('\n}\n', start)]


class RingRegion(unittest.TestCase):
    @unittest.skipUnless(sys.platform == 'darwin', 'clang')
    def test_the_slot_is_the_crop_and_the_present_reads_it_from_the_origin(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'probe.c'
            src.write_text(PROBE)
            exe = Path(tmp) / 'probe'
            subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(NVN), str(src), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
            self.assertIn('present ring region: ok', out)


class RingInTheRenderer(unittest.TestCase):
    src = (NVN / 'nvn_mtl.m').read_text()

    def test_the_copy_takes_the_region_only(self):
        present = function_body(self.src, 'uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {')
        self.assertIn('nvn_ring_region rr = nvn_ring_region_for(rsrc.width, rsrc.height, pring_whole() ? NULL : crop);', present)
        self.assertIn('static int pring_whole(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_PRESENT_RING_WHOLE"); v = e && !strcmp(e, "1"); } return v; }',
                      self.src, 'the whole image only when asked for (a comparison in one build)')
        self.assertIn('MTLOriginMake(rr.x, rr.y, 0), MTLSizeMake(rr.w, rr.h, 1), r, 0, 0, MTLOriginMake(0, 0, 0)', present)
        self.assertNotIn('MTLSizeMake(rsrc.width, rsrc.height, 1)', present, 'never the whole window image again')
        self.assertIn('r.width != rr.w || r.height != rr.h', present, 'the slot has the size of what it holds')
        self.assertIn('g_pring_pic[ring] = rr.picture;', present)

    def test_the_present_reads_the_slot_from_its_origin(self):
        now = function_body(self.src, 'static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4]')
        swap, rebind = now.index('hdr_swap_texture('), now.index('nvn_ring_present_crop(')
        self.assertLess(swap, rebind, 'after the HDR source took the place of the slot')
        self.assertIn('if (ring >= 0 && src == g_pring[ring]) crop = nvn_ring_present_crop(g_pring_pic[ring], src.width, src.height, crop, ring_crop);', now)
        self.assertNotIn('crop', now[:swap].split('{', 1)[1], 'nothing reads the crop before that')

    def test_a_slot_is_busy_from_the_moment_its_ticket_is_taken(self):
        """The slot was free between the queue moving past its ticket and the present marking it busy; a present of
        another size then replaced the texture under the one being shown."""
        link = function_body(self.src, '- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {')
        taken = link.index('g_pq_read = interp >= 0 ? g_pq_in - 1 : g_pq_in;')
        unlock = link.index('pthread_mutex_unlock(&g_pq_mu);', taken)
        self.assertIn('if (ring >= 0) g_pring_busy[ring] = 1;', link[taken:unlock])
        main = function_body(self.src, 'static void *present_main(void *arg) {')
        taken = main.index('uint64_t ticket = ++g_pq_read;')
        unlock = main.index('pthread_mutex_unlock(&g_pq_mu);', taken)
        self.assertIn('if (it.ring >= 0) g_pring_busy[it.ring] = 1;', main[taken:unlock])


if __name__ == '__main__':
    unittest.main()
