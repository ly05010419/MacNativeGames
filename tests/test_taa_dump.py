"""TOTK_TAA_DUMP (diagnostic, 2026-11-25): the edge smoothing's inputs and output of three anti-aliased frames as raw files,
armed in play by "flag taa_dump 1". It must cost nothing unless asked for (no getenv result, no dump), must run whether or
not frame interpolation is on (the MetalFX interpolation dump only runs inside its present path), and must wait for the
GPU before it reads the textures, or it would save the previous frame's contents.
Read, not run: the game run is in the story (docs/superpowers/plans/2026-11-25-botw-edge-smoothing.md, Task 2)."""
from pathlib import Path
import re
import unittest

NVN = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn'
TAA = (NVN / 'totk_taa.m').read_text()
HDR = (NVN / 'totk_taa.h').read_text()
MTL = (NVN / 'nvn_mtl.m').read_text()


def body(text, start):
    i = text.index(start)
    return text[i:text.index('\n}\n', i)]


def real_part(text):   # totk_taa.m: the visionOS stubs come first, the real code after their #else
    return text[text.index('#else', text.index('#if TARGET_OS_VISION')):]


class TaaDump(unittest.TestCase):
    def test_declared(self):
        for decl in ('int taa_dump_due(void);', 'void taa_dump(long frame);', 'void taa_dump_arm(int frames);'):
            self.assertIn(decl, HDR)

    def test_off_without_env(self):
        due = body(real_part(TAA), 'int taa_dump_due(void) {')
        self.assertIn('getenv("TOTK_TAA_DUMP")', due)
        # nothing is dumped unless armed, and only a frame the TAA anti-aliased (taa_frame_end has already moved g.frame on)
        self.assertRegex(due, r'g\.encoded_frame == g\.frame')

    def test_called_at_every_present_after_the_frame_and_waits(self):
        present = body(MTL, 'uint64_t mtl_present(const tex_t *t, double period, const int crop[4]) {')
        self.assertLess(present.index('taa_frame_end();'), present.index('taa_dump_due()'))
        call = present[present.index('taa_dump_due()'):]
        self.assertLess(call.index('mtl_finish_gpu();'), call.index('taa_dump('))
        # not inside the frame interpolation path: that only runs with interpolation on
        self.assertNotIn('taa_dump', body(MTL, 'static int mfx_present_frame_body('))

    def test_flag(self):
        self.assertRegex(MTL, re.escape('if (!strcmp(name, "taa_dump")) {') + r'[^\n]*taa_dump_arm\(')

    def test_visionos_stub(self):
        stubs = TAA[TAA.index('#if TARGET_OS_VISION'):TAA.index('#else', TAA.index('#if TARGET_OS_VISION'))]
        for fn in ('int taa_dump_due(void)', 'void taa_dump(long frame)', 'void taa_dump_arm(int frames)'):
            self.assertIn(fn, stubs)

    def test_files_named_like_the_interpolation_dump(self):
        dump = body(real_part(TAA), 'void taa_dump(long frame) {')
        for name in ('"scene"', '"depth"', '"motion"', '"mask"', '"out"'):
            self.assertIn(name, dump)
        self.assertIn('taa-%llu-%s-%lux%lu', TAA)


if __name__ == '__main__':
    unittest.main()
