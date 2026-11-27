"""Ocarina of Time VR: Link's depth pass and colour pass are posed from the same controller reading.

The colour pass keeps only the pixels whose depth equals the depth pass's. When each draw read the newest controllers, real
controllers (which never hold still) moved the arms between the two passes, no pixel matched, and the hands came out as the flat
colour behind them (2026-11-23, on the Quest; `flag vr_arm_per_draw 1` with `flag vr_arm_jitter 1` shows it again).
"""
import re
import unittest
from pathlib import Path

ARMS = Path(__file__).resolve().parents[1] / 'games/totk/third_party/d2r_gfx/gfx/nvn/oot_vr_arms.inc'


def function_body(text, signature):
    start = text.index(signature)
    open_brace = text.index('{', start)
    depth = 0
    for at in range(open_brace, len(text)):
        depth += {'{': 1, '}': -1}.get(text[at], 0)
        if depth == 0:
            return text[open_brace:at + 1]
    raise AssertionError('unbalanced braces after ' + signature)


class ArmsReadTheControllersOnceAFrame(unittest.TestCase):
    def setUp(self):
        self.text = ARMS.read_text(encoding='utf-8')
        self.reader = function_body(self.text, 'static const oot_arm_input *oot_arm_input_now(void)')

    def test_only_the_frame_reader_touches_the_link(self):
        # the hands come through vr_oot_hands (the controllers turned by the wrist calibration, vr.md row 31), never vr_link_hands
        self.assertNotIn('vr_link_hands(', self.text)
        for call in ('vr_oot_hands(', 'vr_link_pose(', 'vr_link_controls(', 'vr_oot_holding()'):
            uses = [m.start() for m in re.finditer(re.escape(call), self.text)
                    if not self.text[:m.start()].rsplit('\n', 1)[-1].lstrip().startswith(('int ', 'void ', '/*'))]
            self.assertEqual(len(uses), 1, call)
            self.assertIn(call, self.reader, call)

    def test_a_second_draw_of_the_frame_reuses_the_reading(self):
        self.assertRegex(self.reader, r'in\.frame == frame && !atomic_load_explicit\(&g_oot_arm_per_draw')

    def test_the_per_draw_reading_is_off_by_default(self):
        self.assertRegex(self.text, r'static _Atomic int g_oot_arm_per_draw, g_oot_arm_jitter;')

    def test_the_palette_uses_the_frame_reading(self):
        body = function_body(self.text, 'static uint64_t oot_body_palette(const uint8_t *src, uint64_t size)')
        self.assertIn('oot_arm_input_now()', body)


if __name__ == '__main__':
    unittest.main()
