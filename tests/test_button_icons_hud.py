"""make_button_icons.py: the button guide textures (block-linear addressing, outline rasterizer), without game data."""
import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
READY = all(importlib.util.find_spec(m) is not None for m in ('compression.zstd', 'fontTools'))


@unittest.skipUnless(READY, 'needs Python 3.14 and fontTools')
class ButtonGuideTextures(unittest.TestCase):
    def setUp(self):
        import make_button_icons
        self.icons = make_button_icons

    def test_block_linear_covers_the_surface_once(self):
        # 72x72 R8 with 8-GOB blocks, as the game stores Nt_KeyTex*: 2 GOBs wide, 2 blocks high = 16384 bytes
        offsets = {self.icons.gob_offset(x, y, 2, 8) for y in range(72) for x in range(72)}
        self.assertEqual(len(offsets), 72 * 72)
        self.assertTrue(all(0 <= o < 16384 for o in offsets))
        self.assertEqual(self.icons.gob_offset(0, 0, 2, 8), 0)
        self.assertEqual(self.icons.gob_offset(64, 0, 2, 8), 512 * 8)   # the next GOB column starts a block later

    def test_square_with_a_hole(self):
        def draw(pen):   # a square and, reversed, a smaller square inside: nonzero winding leaves it empty
            self.icons.polygon(pen, [(0, 0), (100, 0), (100, 100), (0, 100)])
            self.icons.polygon(pen, [(25, 25), (25, 75), (75, 75), (75, 25)])
        mask = self.icons.rasterize(draw, 72)
        at = lambda x, y: mask[y * 72 + x]
        self.assertEqual(at(6, 6), 255)      # inside the square, past the margin
        self.assertEqual(at(36, 36), 0)      # inside the hole
        self.assertEqual(at(1, 1), 0)        # the margin stays empty
        self.assertEqual(at(6, 65), 255)     # rows run top down: the bottom-left corner is filled too

    def test_hud_textures_of_any_square_size(self):
        # 1.2.1 stores the eight Nt_KeyTex* as 72x72, 1.4.3 as 108x108: a minimal BNTX with each BRTI header the tool reads
        import struct
        for size in (72, 108):
            blob, data_at = bytearray(), {}
            for name in self.icons.HUD_TEXTURES:
                i = len(blob); blob += b'BRTI' + bytes(0x100 - 4)
                struct.pack_into('<H', blob, i + 0x16, 1); struct.pack_into('<I', blob, i + 0x1C, self.icons.R8_UNORM)
                struct.pack_into('<II', blob, i + 0x24, size, size); struct.pack_into('<I', blob, i + 0x34, 3)   # 8-GOB blocks
                n = len(blob); blob += struct.pack('<H', len(name)) + name.encode() + bytes(6); struct.pack_into('<Q', blob, i + 0x60, n)
                p = len(blob); blob += bytes(8); struct.pack_into('<Q', blob, i + 0x70, p); data_at[name] = p
            for name, p in data_at.items():
                struct.pack_into('<Q', blob, p, len(blob)); blob += bytes(((size + 63) // 64) * 512 * 8 * ((size + 63) // 64))
            square = lambda pen: self.icons.polygon(pen, [(0, 0), (100, 0), (100, 100), (0, 100)])
            out = self.icons.redraw_hud(bytes(blob), {cp: (100, square) for cp in self.icons.HUD_TEXTURES.values()})
            centre = struct.unpack_from('<Q', out, next(iter(data_at.values())))[0] + self.icons.gob_offset(size // 2, size // 2, (size + 63) // 64, 8)
            self.assertEqual(out[centre], 255, size)

    def test_petal_symbols_join_a_text_font(self):
        # a minimal CFF font with an X, as the text fonts have: the four symbols come out at new code points, the X unchanged
        import io
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.t2CharStringPen import T2CharStringPen
        from fontTools.ttLib import TTFont
        pen = T2CharStringPen(600, None); self.icons.polygon(pen, [(0, 0), (600, 0), (600, 700), (0, 700)])
        fb = FontBuilder(1000, isTTF=False); fb.setupGlyphOrder(['.notdef', 'X'])
        fb.setupCharacterMap({ord('X'): 'X'}); fb.setupCFF('Test', {}, {'.notdef': T2CharStringPen(500, None).getCharString(), 'X': pen.getCharString()}, {})
        fb.setupHorizontalMetrics({'.notdef': (500, 0), 'X': (600, 0)}); fb.setupHorizontalHeader(); fb.setupOS2(); fb.setupPost(); fb.setupNameTable({})
        out = io.BytesIO(); fb.font.save(out)
        new, added = self.icons.add_petal_glyphs(out.getvalue(), list(self.icons.PETAL_GLYPHS))
        self.assertEqual(added, 4)
        font = TTFont(io.BytesIO(new)); cmap = font.getBestCmap(); glyphs = font.getGlyphSet()
        self.assertTrue(set(self.icons.PETAL_GLYPHS) <= set(cmap))
        self.assertEqual(font['maxp'].numGlyphs, 6)
        from fontTools.pens.boundsPen import BoundsPen
        for cp in self.icons.PETAL_GLYPHS:   # each symbol about as tall as the X and centred on it (the triangle on its centroid)
            box = BoundsPen(glyphs); glyphs[cmap[cp]].draw(box); y0, y1 = box.bounds[1], box.bounds[3]
            self.assertAlmostEqual((y0 + y1) / 2, 350, delta=80); self.assertTrue(600 < y1 - y0 < 800, (cp, y1 - y0))
        self.assertEqual(self.icons.add_petal_glyphs(new, list(self.icons.PETAL_GLYPHS)), (new, 0))   # a second run adds nothing


if __name__ == '__main__':
    unittest.main()
