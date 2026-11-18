"""Breath of the Wild's scene size on iPhone / iPad (game_patches_botw.c): both display-mode pairs come from the data page
after main, so the pane's custom scene (TOTK_RENDER_RES, user 2026-11-17) takes effect without new code on the device,
which cannot write code. The patched load must read the slot the start writes, and no other patch may own that slot."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'games/totk/runtime/game_patches_botw.c'


def ldr_x_offset(word):
    """LDR Xt, [Xn, #imm] (unsigned offset): (rt, rn, byte offset), or None for another instruction."""
    if word & 0xffc00000 != 0xf9400000:
        return None
    return word & 31, (word >> 5) & 31, ((word >> 10) & 0xfff) * 8


def body(text, name):
    return re.search(r'static void ' + name + r'\(Module \*m\) \{.*?\n}', text, re.S).group(0)


class PhoneScene(unittest.TestCase):
    def setUp(self):
        self.text = SOURCE.read_text()

    def check_pair(self, name, screen):
        fn = body(self.text, name)
        load = re.search(r'put_word\(m, \w+(?: \+ 4)?, (0x[0-9a-f]+)u, "(?:docked|handheld) render size"\);\s*/\* ldr', fn)
        self.assertIsNotNone(load, f'{name}: the pair load')
        rt, rn, offset = ldr_x_offset(int(load.group(1), 16))
        self.assertEqual((rt, rn), (8, 8), f'{name}: x8 holds the pair the game stores next (str x8)')
        store = re.search(r'atomic_store\(\(_Atomic uint64_t \*\)\(void \*\)\(page \+ (\w+)\), output_floats\((\d+), render\)\)', fn)
        self.assertIsNotNone(store, f'{name}: the start writes the pair')
        self.assertEqual(int(store.group(1), 0), offset, f'{name}: the load reads the slot the start writes')
        self.assertEqual(int(store.group(2)), screen, f'{name}: the screen width of that mode\'s canvas')
        # the custom scene reaches this mode, held at this mode's canvas (the pair's screen width)
        self.assertIn(f'phone_scene_custom({screen})', fn, f'{name}: the custom scene, held at the {screen}-wide canvas')
        return offset

    def test_both_modes_read_their_own_slot(self):
        docked = self.check_pair('patch_render_phone', 1920)
        handheld = self.check_pair('patch_phone_render', 1280)
        # The other slots of the page: real-time frame step +0, pad tick +8, ultrawide +0x20 (both modes run on the phone).
        taken = {0, 8, 0x20}
        for slot in (docked, handheld):
            self.assertNotIn(slot, taken)
        self.assertNotEqual(docked, handheld)
        for found in re.findall(r'\(page \+ (\w+)\)', self.text):
            self.assertIn(int(found, 0), taken | {docked, handheld}, f'a page slot nobody listed: +{found}')

    def test_custom_sizes_stay_within_each_modes_canvas(self):
        # Above the canvas the game cannot make its heaps: docked's canvas is 1920x1080, handheld's 1280x720. The list
        # goes up to the docked canvas for faster devices (user 2026-11-17); a size above a mode's canvas is held there.
        fn = body_fn(self.text, 'phone_scene_custom')
        sizes = re.search(r'sizes\[\] = \{(.*?)\};', fn, re.S).group(1)
        pairs = re.findall(r'\{"(\d+)x(\d+)", (\d+)\}', sizes)
        self.assertEqual([p[0] for p in pairs], ['1920', '1600', '1280', '960', '800', '640'])
        for w, h, width in pairs:
            self.assertEqual(w, width)
            self.assertEqual(int(w) * 9 // 16, int(h), '16:9, as the render height follows the width')
        self.assertIn('if (sizes[i].width <= canvas_w) return sizes[i].width;', fn)
        self.assertIn('return canvas_w;', fn, 'a size above the canvas is held at the canvas, never above it')


def body_fn(text, name):
    return re.search(r'static unsigned ' + name + r'\(unsigned canvas_w\) \{.*?\n}', text, re.S).group(0)


if __name__ == '__main__':
    unittest.main()
