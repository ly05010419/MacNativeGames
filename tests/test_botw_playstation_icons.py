"""botw_playstation_icons.py and make_botw_button_icons.py --style playstation: the PlayStation symbols drawn over the
button glyphs of External_00.bffnt's BC4 sheet and the action guide's R8 key textures, each file keeping its size. A
synthetic font and texture archive stand in for the game's."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
import botw_playstation_icons as ps         # noqa: E402
import make_botw_button_icons as icons      # noqa: E402
from test_botw_button_icons import at, flyt, sarc   # noqa: E402

CELL, COLUMNS, ROWS = 30, 2, 5


def bntx(members):
    """A BNTX holding textures (name -> (format, width, height, image bytes)), each BRTI 0x100 apart, layout word 0 (block
    height 1)."""
    head = bytearray(b'BNTX' + bytes(12))
    body = bytearray()
    base = 0x10 + 0x100 * len(members)
    images = bytearray()
    for k, (name, (fmt, w, h, image)) in enumerate(members.items()):
        brti = bytearray(0x100)
        brti[:4] = b'BRTI'
        struct.pack_into('<HHHIIIIIIII', brti, 0x12, 0, 0, 1, 0, fmt, 0, w, h, 1, 1, 0)
        struct.pack_into('<I', brti, 0x50, len(image))
        here = 0x10 + 0x100 * k
        struct.pack_into('<Q', brti, 0x60, here + 0x80)
        struct.pack_into('<H', brti, 0x80, len(name)); brti[0x82:0x82 + len(name)] = name.encode()
        struct.pack_into('<Q', brti, 0x70, here + 0xc0)
        struct.pack_into('<Q', brti, 0xc0, base + len(images))
        body += brti
        images += image + bytes(-len(image) % 0x10)
    return bytes(head + body + images)


def disc(size, hole=True):
    """Upright rows of a white disc with a dark square letter in the middle (the game's icons in miniature)."""
    c, r = size / 2, size * 0.42
    rows = [bytearray(255 if (x + 0.5 - c) ** 2 + (y + 0.5 - c) ** 2 <= r * r else 0 for x in range(size))
            for y in range(size)]
    if hole:
        for y in range(size // 2 - 3, size // 2 + 3):
            rows[y][size // 2 - 3:size // 2 + 3] = bytes(6)
    return rows


def sheet_bffnt():
    """A BFFNT with a TGLP (one BC4 sheet of 2 x 5 cells of 30x30) and a CMAP U+E040-E04C, each glyph a disc."""
    width, height = 1 + COLUMNS * (CELL + 1), ROWS * (CELL + 1)
    width, height = width + -width % 4, height + -height % 4
    blocks = (width // 4, height // 4)
    image = bytearray(((blocks[0] * 8 + 63) // 64) * 512 * ((blocks[1] + 7) // 8))
    tex = bntx({'Sheet': (ps.BC4_UNORM, width, height, bytes(image))})
    codes = sorted(ps.GLYPH_SYMBOL)
    lo, hi = 0xE040, 0xE04C
    index = [codes.index(c) if c in codes else 0xFFFF for c in range(lo, hi + 1)]
    table = struct.pack(f'<{len(index)}H', *index)
    tglp_size, cmap_size = 0x20, 24 + len(table) + (-len(table) % 4)
    sheet_at = 0x14 + tglp_size + cmap_size
    tglp = b'TGLP' + struct.pack('<I', tglp_size) + struct.pack('<BBBBIHHHHHHI', CELL, CELL, 1, 0, len(tex), 0, 0, COLUMNS,
                                                                ROWS, 0, 0, sheet_at)
    tglp += bytes(tglp_size - len(tglp))
    cmap = b'CMAP' + struct.pack('<IIIHHI', cmap_size, lo, hi, 1, 0, 0) + table
    cmap += bytes(cmap_size - len(cmap))
    ffnt = bytearray(b'FFNT\xfe\xff' + struct.pack('<HIIHH', 0x14, 0x04010000, sheet_at + len(tex), 2, 0) + tglp + cmap + tex)
    sheet = ps.Sheet(bytes(ffnt)); a, n = sheet.image(); pixels = bytearray(ffnt[a:a + n])
    for code in codes:
        sheet.put(pixels, code, disc(CELL))
    ffnt[a:a + n] = pixels
    return bytes(ffnt)


class Blocks(unittest.TestCase):
    def test_bc4_keeps_black_white_and_the_greys_close(self):
        for values in ([0] * 8 + [255] * 8, [0, 255, 100, 140] * 4, list(range(0, 256, 17)), [77] * 16):
            out = ps.bc4_decode(ps.bc4_encode(values))
            self.assertTrue(all(abs(a - b) <= 20 for a, b in zip(out, values)), (values, out))
            self.assertEqual([o for o, v in zip(out, values) if v in (0, 255)], [v for v in values if v in (0, 255)])

    def test_a_cell_reads_back_as_written(self):
        ffnt = sheet_bffnt(); sheet = ps.Sheet(ffnt); a, n = sheet.image()
        self.assertEqual(sheet.cell(ffnt[a:a + n], 0xE040), disc(CELL))


class Drawing(unittest.TestCase):
    def test_the_letter_is_filled_and_the_symbol_cut(self):
        middle = CELL // 2
        cross, circle = ps.redraw(disc(CELL), 'cross'), ps.redraw(disc(CELL), 'circle')
        self.assertLess(cross[middle][middle], 40)                  # the cross meets in the middle
        self.assertEqual(circle[middle][middle], 255)              # the letter's hole filled, inside the ring
        self.assertLess(min(circle[middle][middle - 7:middle + 8]), 60)   # the ring
        self.assertEqual((cross[0][0], circle[0][0]), (0, 0))      # the outside stays dark

    def test_every_symbol_draws_inside_its_shape(self):
        for symbol in list(ps.GLYPH_SYMBOL.values()):
            out = ps.redraw(disc(CELL), symbol)
            self.assertTrue(all(o <= v for r, rr in zip(out, ps.silhouette(disc(CELL))) for o, v in zip(r, rr)), symbol)
            self.assertNotEqual(out, ps.silhouette(disc(CELL)), symbol)


class Font(unittest.TestCase):
    def test_only_the_sheet_changes_and_the_size_stays(self):
        ffnt = sheet_bffnt(); cache = {}
        out = ps.font(ffnt, cache)
        a, b = ps.sheet_span(ffnt)
        self.assertEqual((len(out), out[:a], out[b:]), (len(ffnt), ffnt[:a], ffnt[b:]))
        sheet = ps.Sheet(out)
        cells = {c: sheet.cell(out[a:b], c) for c in ps.GLYPH_SYMBOL}
        self.assertLess(cells[0xE040][CELL // 2][CELL // 2], 60)            # Cross
        self.assertGreater(cells[0xE041][CELL // 2][CELL // 2], 200)        # Circle
        self.assertEqual(ps.font(ffnt, cache), out)                          # the cache gives the same glyphs

    def test_its_own_archive_with_a_drawn_sheet_and_a_remap_is_its_own(self):
        ffnt = sheet_bffnt(); archive = sarc({'Ancient_00.bffnt': b'ancient', icons.MEMBER: ffnt})
        a, b = icons.sarc_member(archive, icons.MEMBER)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'Font_EU.sbfarc'
            mine = archive[:a] + icons.font_remap(ffnt, {0xE040: 0xE041}, 'playstation') + archive[b:]
            path.write_bytes(icons.yaz0_stored(mine, 0x2000))
            self.assertTrue(icons.own(path, archive))
            other = bytearray(mine); other[a + 2] ^= 1                   # a byte of the header: another mod's
            path.write_bytes(icons.yaz0_stored(bytes(other), 0x2000))
            self.assertFalse(icons.own(path, archive))


class Guide(unittest.TestCase):
    def key_bntx(self):
        image = bytes(512 * 6)       # 48x48 R8: one GOB wide, six GOBs high (block height 1)
        names = ['Nt_KeyTexA_00^d', 'Nt_KeyTexB_00^d', 'Nt_KeyTexX_00^d', 'Nt_KeyTexY_00^d', 'Nt_KeyTexL_00^d',
                 'Nt_KeyTexZL_00^d', 'Nt_KeyTexLSt_00^d']
        out = bytearray(bntx({n: (ps.R8_UNORM, 48, 48, image) for n in names}))
        found = ps.textures(bytes(out))
        for n in names:
            ps.r8_write(out, found[n], disc(48))
        return bytes(out), found

    def test_the_face_keys_take_the_symbols_and_the_shoulders_their_driver(self):
        old, found = self.key_bntx()
        read = lambda b, n: ps.r8_read(b, found[n])  # noqa: E731
        new = ps.layout_textures(old, {})
        self.assertEqual(len(new), len(old))
        for n in ('Nt_KeyTexA_00^d', 'Nt_KeyTexB_00^d', 'Nt_KeyTexX_00^d', 'Nt_KeyTexY_00^d', 'Nt_KeyTexL_00^d',
                  'Nt_KeyTexZL_00^d'):
            self.assertNotEqual(read(new, n), read(old, n), n)
        self.assertEqual(read(new, 'Nt_KeyTexLSt_00^d'), read(old, 'Nt_KeyTexLSt_00^d'))     # a stick stays
        r2 = ps.layout_textures(old, {0xE044: 0xE047})                 # R2 drives L: its key reads R2
        self.assertEqual(read(r2, 'Nt_KeyTexL_00^d'), ps.redraw(read(old, 'Nt_KeyTexL_00^d'), 'R2'))
        face = ps.layout_textures(old, {0xE044: 0xE040})               # a face button drives L: the key stays
        self.assertEqual(read(face, 'Nt_KeyTexL_00^d'), read(old, 'Nt_KeyTexL_00^d'))

    def test_the_diamonds_letters_are_blank(self):
        out = icons.diamond(flyt(), 'right', 'bottom', named=False)
        self.assertEqual([at(out, n)[-1] for n in ('T_B_00', 'T_Y_00', 'T_X_00')], [' ', ' ', ' '])
        self.assertEqual(at(out, 'N_BtnA_00')[:2], (0.0, -26.0))       # the button still moves


class Style(unittest.TestCase):
    def test_the_default_follows_the_controller_type(self):
        with tempfile.TemporaryDirectory() as tmp:
            kind = Path(tmp)/'controller-type.txt'
            self.assertEqual(icons.default_style(kind), 'xbox')                # no type file: auto
            for word, style in (('xbox\n', 'xbox'), ('auto\n', 'xbox'), ('playstation', 'playstation')):
                kind.write_text(word)
                self.assertEqual(icons.default_style(kind), style)


if __name__ == '__main__':
    unittest.main()
