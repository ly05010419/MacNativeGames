"""Breath of the Wild prompt tool (games/botw/scripts/botw_formats.py, botw_prompts.py) on small made-up archives: no game data."""
import io
import math
import random
import re
import string
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
sys.path.insert(0, str(ROOT/'games/botw/scripts'))
sys.path.insert(0, str(ROOT/'games/totk/scripts'))
try:
    import botw_formats as bf
    import botw_prompts as bp
except (SystemExit, ImportError):     # make_romfs_patches needs Python 3.14 (compression.zstd)
    bf = bp = None
try:
    import make_button_icons as mbi         # the PlayStation drawings; it needs fontTools and exits without it
except (SystemExit, ImportError):
    mbi = None
import prompt_layout as pl      # noqa: E402

NAMES = ['Nt_KeyTexA_00^d', 'Nt_KeyTexB_00^d', 'Nt_KeyTexX_00^d', 'Nt_KeyTexY_00^d', 'Nt_KeyTexL_00^d', 'Nt_KeyTexZL_00^d',
         'Nt_KeyTexLSt_00^d', 'Nt_KeyTexLSt_01^d']


def yaz0_encode(data):
    """A plain greedy Yaz0 encoder (back references up to 4096 bytes), enough to test the splice against references."""
    out, i, n, seen = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8)), 0, len(data), {}
    while i < n:
        code, body = 0, bytearray()
        for bit in range(8):
            if i >= n:
                break
            best, dist = 0, 0
            for j in reversed(seen.get(data[i:i + 3], [])[-16:]):
                if i - j > 4096:
                    break
                length = 3
                while i + length < n and length < 0x111 and data[j + length] == data[i + length]:
                    length += 1
                if length > best:
                    best, dist = length, i - j
            step = 1
            if best >= 3:
                d = dist - 1
                if best < 18:
                    body += bytes([(best - 2) << 4 | d >> 8, d & 255])
                else:
                    body += bytes([d >> 8, d & 255, best - 18])
                step = best
            else:
                code |= 0x80 >> bit; body.append(data[i])
            for k in range(i, i + step):
                seen.setdefault(data[k:k + 3], []).append(k)
            i += step
        out.append(code); out += body
    return bytes(out)


def sample(size, seed=1):
    rng = random.Random(seed)
    words = [bytes(rng.randrange(256) for _ in range(rng.randrange(3, 40))) for _ in range(30)]
    out = bytearray()
    while len(out) < size:
        out += rng.choice(words) if rng.random() < 0.8 else bytes([rng.randrange(256)])
    return bytes(out[:size])


def sarc(members):
    """A little-endian SARC (the layout make_romfs_patches.sarc_members reads), members 8-byte aligned."""
    names, table, data = bytearray(), bytearray(), bytearray()
    for name, blob in members.items():
        while len(data) % 8:
            data += b'\0'
        table += struct.pack('<4I', 0, 0x01000000 | len(names) // 4, len(data), len(data) + len(blob))
        names += name.encode() + b'\0'
        while len(names) % 4:
            names += b'\0'
        data += blob
    head = b'SFAT' + struct.pack('<HHI', 12, len(members), 0x65) + bytes(table) + b'SFNT' + struct.pack('<HH', 8, 0) + bytes(names)
    start = 0x14 + len(head); start += -start % 8
    out = bytearray(b'SARC' + struct.pack('<H', 0x14) + b'\xff\xfe' + struct.pack('<IIHH', 0, start, 0x100, 0) + head)
    out += b'\0' * (start - len(out)) + data
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


def bntx(textures, fmt=bf.R8_UNORM if bf else 0x201, size=48):
    """A BNTX holding one BRTI per name, each a size x size R8 block-linear texture (block height 8, 4096 bytes), as in the game."""
    head = bytearray(b'BNTX' + bytes(0x1fc)); names = bytearray(); brti = bytearray(); pointers = bytearray(); data = bytearray()
    base = len(head)
    for i, (name, rows) in enumerate(textures.items()):
        raw = name.encode()
        name_at = base + len(names); names += struct.pack('<H', len(raw)) + raw + b'\0' * (-(2 + len(raw)) % 8)
        entry = bytearray(0xa0)
        entry[:4] = b'BRTI'
        struct.pack_into('<HHHIIIIIIII', entry, 0x12, 0, 0, 1, 1, fmt, 0, size, size, 1, 1, 3)
        struct.pack_into('<I', entry, 0x50, 4096)
        struct.pack_into('<Q', entry, 0x60, name_at)
        brti += entry; pointers += struct.pack('<Q', 0)
        image = bytearray(4096)
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                image[bf.r8_address(x, y, size, 8)] = v
        data += image
    brti_at = base + len(names); pointers_at = brti_at + len(brti); data_at = pointers_at + len(pointers)
    out = bytearray(head + names + brti + pointers + data)
    for i in range(len(textures)):
        o = brti_at + i * 0xa0
        struct.pack_into('<Q', out, o + 0x70, pointers_at + i * 8)
        struct.pack_into('<Q', out, pointers_at + i * 8, data_at + i * 4096)
    return bytes(out)


def picture(seed):
    rng = random.Random(seed)
    return [bytes(rng.randrange(256) for _ in range(48)) for _ in range(48)]


def key_picture(letter_box=(20, 16, 28, 30)):
    """A white key (a block of 255s with an empty margin) with a dark letter-like mark inside, and a long dark outline."""
    rows = [bytearray(48) for _ in range(48)]
    for y in range(6, 42):
        for x in range(6, 42):
            rows[y][x] = 255
    for x in range(6, 42):          # the outline: a long dark line, longer than any letter
        rows[44][x] = 0
    x0, y0, x1, y1 = letter_box
    for y in range(y0, y1):
        for x in range(x0, x1):
            rows[y][x] = 0
    return [bytes(r) for r in rows]


def disc_picture(cx, cy, radius, size=48):
    """A black picture with a white disc of the given centre and radius and a dark mark inside, like the game's face buttons."""
    rows = [bytearray(size) for _ in range(size)]
    for y in range(size):
        for x in range(size):
            if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= radius ** 2:
                rows[y][x] = 255
    for y in range(int(cy) - 4, int(cy) + 5):
        for x in range(int(cx) - 3, int(cx) + 4):
            rows[y][x] = 0
    return [bytes(r) for r in rows]


def same_disc(test, new, old, tolerance=2):
    """The white disc of `new` has the box of the disc of `old` within `tolerance` pixels."""
    for a, b in zip(bp.disc_of(new), bp.disc_of(old)):
        test.assertAlmostEqual(a, b, delta=tolerance)


_STUB_FONT = []


def stub_font():
    """A tiny TrueType font (made up: every capital letter and digit is a clockwise square, like the Switch system font's outer
    contours), the `font` the button drawings take their letters from."""
    if not _STUB_FONT:
        from fontTools.fontBuilder import FontBuilder
        from fontTools.pens.ttGlyphPen import TTGlyphPen
        chars = string.ascii_uppercase + string.digits
        names = ['.notdef'] + [f'g{ord(c)}' for c in chars]
        builder = FontBuilder(1000, isTTF=True)
        builder.setupGlyphOrder(names)
        builder.setupCharacterMap({ord(c): f'g{ord(c)}' for c in chars})
        glyphs = {}
        for name in names:
            pen = TTGlyphPen(None)
            pen.moveTo((100, 0)); pen.lineTo((100, 700)); pen.lineTo((500, 700)); pen.lineTo((500, 0)); pen.closePath()
            glyphs[name] = pen.glyph()
        builder.setupGlyf(glyphs)
        builder.setupHorizontalMetrics({name: (600, 100) for name in names})
        builder.setupHorizontalHeader(ascent=800, descent=-200)
        builder.setupNameTable({'familyName': 'Stub', 'styleName': 'Regular'})
        builder.setupOS2(); builder.setupPost()
        _STUB_FONT.append(builder.font)
    return _STUB_FONT[0]


class StubLetters:
    @property
    def font(self):
        return stub_font()

    def text(self, label, cap, cx, cy, max_width):    # a square of side `cap`, so the new "letter" shows where it was put
        h = cap / 2
        return [[(cx - h, cy - h), (cx + h, cy - h), (cx + h, cy + h), (cx - h, cy + h)]]


@unittest.skipIf(bf is None, 'needs Python 3.14')
class Yaz0Splice(unittest.TestCase):
    def setUp(self):
        self.data = sample(60000)
        self.stream = yaz0_encode(self.data)

    def test_encoder_round_trips_and_uses_references(self):
        self.assertEqual(bf.yaz0_decode(self.stream), self.data)
        self.assertLess(len(self.stream), len(self.data))

    def test_splice_decodes_to_the_edit_and_keeps_both_ends_of_the_stream(self):
        edit = bytes(range(200))
        result = bf.yaz0_splice(self.stream, self.data, [(30000, edit)])
        expected = self.data[:30000] + edit + self.data[30200:]
        self.assertEqual(bf.yaz0_decode(result), expected)
        self.assertEqual(result[:600], self.stream[:600])         # before the edit: the original stream
        self.assertEqual(result[-600:], self.stream[-600:])       # well after it: the original stream again
        self.assertLess(len(result), len(self.stream) + 12000)    # only the stretch around the edit (about 8 KB) became literals

    def test_two_edits_far_apart_and_two_near_each_other(self):
        edits = [(5000, b'A' * 10), (5100, b'B' * 10), (50000, b'C' * 300)]
        expected = bytearray(self.data)
        for o, b in edits:
            expected[o:o + len(b)] = b
        self.assertEqual(bf.yaz0_decode(bf.yaz0_splice(self.stream, self.data, edits)), bytes(expected))

    def test_edits_a_little_further_apart_than_the_reach(self):
        for gap in (bf.REACH + 1, bf.REACH + 50, bf.REACH + 400, bf.REACH + 1500):
            edits = [(20000, b'P' * 4), (20000 + gap, b'Q' * 4)]
            expected = bytearray(self.data)
            for o, b in edits:
                expected[o:o + len(b)] = b
            with self.subTest(gap=gap):
                self.assertEqual(bf.yaz0_decode(bf.yaz0_splice(self.stream, self.data, edits)), bytes(expected))

    def test_edit_at_the_very_start_and_the_very_end(self):
        for offset in (0, len(self.data) - 5):
            result = bf.yaz0_splice(self.stream, self.data, [(offset, b'\x99' * 5)])
            self.assertEqual(bf.yaz0_decode(result)[offset:offset + 5], b'\x99' * 5)

    def test_a_wrong_decoded_copy_is_caught(self):      # negative control: the final decode check must see a mismatch
        wrong = bytearray(self.data); wrong[100] ^= 1
        with self.assertRaises(ValueError):
            bf.yaz0_splice(self.stream, bytes(wrong), [(30000, b'x')])

    def test_no_edits_gives_the_stream_back(self):
        self.assertEqual(bf.yaz0_splice(self.stream, self.data, []), self.stream)


@unittest.skipIf(bf is None, 'needs Python 3.14')
class Textures(unittest.TestCase):
    def test_swizzle_is_a_permutation_of_the_image(self):
        addresses = {bf.r8_address(x, y, 48, 8) for x in range(48) for y in range(48)}
        self.assertEqual(len(addresses), 48 * 48)
        self.assertLess(max(addresses), 4096)

    def test_read_write_round_trip(self):
        pics = {n: picture(i) for i, n in enumerate(NAMES)}
        blob = bntx(pics)
        textures = bf.bntx_textures(blob)
        self.assertEqual(list(textures), NAMES)
        for name in NAMES:
            self.assertEqual(bf.r8_read(blob, textures[name]), pics[name])
        new = picture(99)
        image = bf.r8_image(blob, textures[NAMES[2]], new)
        self.assertEqual(len(image), 4096)
        patched = bytearray(blob); patched[textures[NAMES[2]].offset:textures[NAMES[2]].offset + 4096] = image
        self.assertEqual(bf.r8_read(bytes(patched), textures[NAMES[2]]), new)
        self.assertEqual(bf.r8_read(bytes(patched), textures[NAMES[0]]), pics[NAMES[0]])    # the others did not move

    def test_other_formats_and_sizes_are_refused(self):
        blob = bntx({NAMES[0]: picture(1)}, fmt=0x1d01)           # BC1, not R8
        with self.assertRaises(ValueError):
            bf.r8_read(blob, bf.bntx_textures(blob)[NAMES[0]])
        good = bntx({NAMES[0]: picture(1)}); texture = bf.bntx_textures(good)[NAMES[0]]
        with self.assertRaises(ValueError):
            bf.r8_image(good, texture, picture(1)[:47])          # not 48 rows
        with self.assertRaises(ValueError):
            bf.bntx_textures(b'NOPE' + bytes(100))

    def test_a_letter_is_found_and_an_outline_is_not(self):
        marks = bf.enclosed_marks(key_picture())
        self.assertEqual(len(marks), 1)
        self.assertEqual(min(p[0] for p in marks[0]), 20)

    def test_coverage_fills_a_shape_and_a_reversed_contour_cuts_a_hole(self):
        square = [(10, 10), (30, 10), (30, 30), (10, 30)]
        hole = [(15, 15), (15, 25), (25, 25), (25, 15)]          # wound the other way
        full = bf.coverage([square], 48)
        self.assertEqual((full[20][20], full[5][5], full[20][9]), (255, 0, 0))
        cut = bf.coverage([square, hole], 48)
        self.assertEqual((cut[20][20], cut[12][12]), (0, 255))

    def test_curves_are_flattened(self):
        ops = [('moveTo', ((0, 0),)), ('qCurveTo', ((10, 10), (20, 0))), ('lineTo', ((0, 0),)), ('closePath', ())]
        contour = bf.flatten(ops)[0]
        self.assertGreater(len(contour), 10)
        self.assertEqual(contour[12], (20.0, 0.0))


def bc4_bntx(width, height, block_height, rows):
    """A BNTX with one BC4 block-linear texture holding `rows` (a list of bytearray rows), as inside a BFFNT's glyph sheet."""
    blocks_w, blocks_h = width // 4, height // 4
    gobs = -(-blocks_h // (8 * block_height)) * block_height * (width * 2 // 64)
    image = bytearray(gobs * 512)
    for by in range(blocks_h):
        for bx in range(blocks_w):
            at = bf.gob_address(bx, by, blocks_w, 8, block_height)
            image[at:at + 8] = bf.bc4_encode([rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)])
    head = bytearray(b'BNTX' + bytes(0x1fc)); name = struct.pack('<H', 1) + b'x' + bytes(5)
    entry = bytearray(0xa0); entry[:4] = b'BRTI'
    struct.pack_into('<HHHIIIIIIII', entry, 0x12, 0, 0, 1, 1, bf.BC4_UNORM, 0, width, height, 1, 1, block_height.bit_length() - 1)
    struct.pack_into('<I', entry, 0x50, len(image))
    name_at, brti_at = len(head), len(head) + len(name)
    pointers_at = brti_at + len(entry); data_at = pointers_at + 8
    struct.pack_into('<Q', entry, 0x60, name_at); struct.pack_into('<Q', entry, 0x70, pointers_at)
    return bytes(head + name + entry + struct.pack('<Q', data_at) + image)


def bffnt(rows, columns=5, grid_rows=4, cell=50, block_height=1, codes=(0xE040, 0xE041, 0xE042, 0xE043), cell_height=None):
    """A BFFNT: header, TGLP with the sheet (a BNTX) and a CMAP table starting at 0xE040 (glyph i = code 0xE040 + i). Cells are
    cell x cell_height pixels (default square)."""
    cell_height = cell_height or cell
    sheet = bc4_bntx(256, grid_rows * (cell_height + 1) + 4, block_height, rows)
    tglp_at = 0x14
    sheet_at = 0x1000
    tglp = b'TGLP' + struct.pack('<I', sheet_at + len(sheet) - tglp_at) + struct.pack('<BBBBIHHHHHHI', cell, cell_height, 1, cell, len(sheet), 36, 12, columns, grid_rows, 256, grid_rows * (cell_height + 1) + 4, sheet_at)
    table = struct.pack('<%dH' % len(codes), *range(len(codes)))
    cmap = b'CMAP' + struct.pack('<I', 24 + len(table)) + struct.pack('<IIHHI', codes[0], codes[-1], 1, 0, 0) + table
    out = bytearray(b'FFNT' + b'\xff\xfe' + struct.pack('<H', 0x14) + bytes(4) + bytes(4) + struct.pack('<H', 2) + bytes(2))
    body = bytearray(out) + bytearray(tglp.ljust(sheet_at - len(out), b'\0')) + bytearray(sheet)
    cmap_at = len(body); body += cmap
    # the sections are found by walking sizes: TGLP's size reaches the CMAP
    struct.pack_into('<I', body, tglp_at + 4, cmap_at - tglp_at)
    return bytes(body)


@unittest.skipIf(bf is None, 'needs Python 3.14')
class FontGlyphs(unittest.TestCase):
    def sheet(self):
        rng = random.Random(3)
        rows = [bytearray(rng.choice((0, 255, 128)) for _ in range(256)) for _ in range(4 * 51 + 4)]
        return rows

    def test_bc4_block_round_trip_of_flat_and_two_level_blocks(self):
        for values in ([0] * 16, [255] * 16, [0, 255] * 8, [7] * 16):
            self.assertEqual(bf.bc4_decode(bf.bc4_encode(values)), values)

    def test_the_sheet_reads_back_and_a_cell_is_upright(self):
        rows = self.sheet()
        data = bffnt(rows)
        font = bf.Bffnt(data)
        self.assertEqual((font.cell, font.columns, font.rows), (50, 5, 4))
        self.assertEqual(sorted(font.cells), [0xE040, 0xE041, 0xE042, 0xE043])
        decoded = bf.bc4_read(data[font.sheet_at:], font.texture)
        x0, y0, x1, y1 = font.cell_box(0xE042)
        self.assertEqual(font.get(decoded, 0xE042), [bytes(r[x0:x1]) for r in reversed(decoded[y0:y1])])
        self.assertEqual(font.cell_box(0xE040)[:2], (1, 4 * 51 + 4 - 51))     # glyph 0: top left upright, behind a one-pixel gap

    def test_only_the_blocks_under_the_changed_cell_are_written_again(self):
        data = bffnt(self.sheet())
        font = bf.Bffnt(data)
        sheet = data[font.sheet_at:]
        rows = bf.bc4_read(sheet, font.texture)
        box = font.put(rows, 0xE041, [bytes([200]) * 50] * 50)
        image = bf.bc4_image(sheet, font.texture, rows, [box])
        old = sheet[font.texture.offset:font.texture.offset + font.texture.image_size]
        changed = [i for i in range(len(image)) if image[i] != old[i]]
        self.assertTrue(changed)
        self.assertLess(len(changed), len(image) // 3)                       # one cell of twenty: most blocks are untouched
        after = bf.bc4_read(sheet[:font.texture.offset] + image + sheet[font.texture.offset + len(image):], font.texture)
        self.assertEqual(font.get(after, 0xE041)[10][10], 200)
        self.assertEqual(font.get(after, 0xE042), font.get(rows, 0xE042))    # the neighbour cell is as it was

    def test_a_neighbour_pixel_in_a_shared_block_keeps_its_value_only_if_the_new_block_can_hold_it(self):
        # Cells are 50 pixels on a 51-pixel step, so a 4x4 block can hold pixels of two cells. Such a block is encoded again
        # with the new cell's extremes: a blank (0) neighbour pixel stays exact, a mid grey can move to the nearest of the
        # eight levels. The tool keeps this limit: in the game's seven fonts the changed glyphs' blocks lose no neighbour value.
        for edge, want in ((0, 0), (120, 109)):
            rows = [bytearray(256) for _ in range(4 * 51 + 4)]
            data = bffnt(rows)
            font = bf.Bffnt(data)
            x0, y0, x1, y1 = font.cell_box(0xE042)
            for y in range(y0, y1):
                rows[y][x0:x0 + 2] = bytes([edge, edge])        # the left edge of the neighbour, in the block it shares
            data = bffnt(rows)
            sheet = data[font.sheet_at:]
            rows = bf.bc4_read(sheet, font.texture)
            box = font.put(rows, 0xE041, [bytes([255]) * 50] * 50)          # the gap column between the cells stays 0
            image = bf.bc4_image(sheet, font.texture, rows, [box])
            after = bf.bc4_read(sheet[:font.texture.offset] + image + sheet[font.texture.offset + len(image):], font.texture)
            with self.subTest(edge=edge):
                self.assertEqual(rows[y0 + 10][x0], edge)
                self.assertEqual(after[y0 + 10][x0], want)

    def test_a_cell_of_the_wrong_size_and_a_wrong_format_are_refused(self):    # negative controls
        data = bffnt(self.sheet())
        font = bf.Bffnt(data)
        with self.assertRaises(ValueError):
            font.put(bf.bc4_read(data[font.sheet_at:], font.texture), 0xE040, [bytes(50)] * 49)
        r8 = bntx({NAMES[0]: picture(1)}); texture = bf.bntx_textures(r8)[NAMES[0]]
        with self.assertRaises(ValueError):
            bf.bc4_read(r8, texture)
        with self.assertRaises(ValueError):
            bf.Bffnt(b'NOPE' + bytes(100))

    @unittest.skipIf(mbi is None, 'needs fontTools (the button drawings come from make_button_icons.py)')
    def test_make_font_redraws_the_face_glyphs_and_keeps_the_rest_of_the_archive(self):
        rows = self.sheet()
        data = bffnt(rows)
        archive = sarc({'External_00.bffnt': data, 'Other.bffnt': sample(2000, 9)})
        stream = yaz0_encode(archive)
        letters = StubLetters()
        new_stream, cells, _, grown = bp.make_font(stream, pl.prompt_controls('', 'position'), letters)
        self.assertEqual(sorted(cells), [0xE040, 0xE041, 0xE042, 0xE043])    # the synthetic font has no other glyphs
        self.assertEqual(grown, 0)                                           # scale 1: the archive keeps its size
        new_archive = bf.yaz0_decode(new_stream)
        self.assertEqual(len(new_archive), len(archive))
        before, after = bf.sarc_members(archive), bf.sarc_members(new_archive)
        self.assertEqual(archive[slice(*before['Other.bffnt'])], new_archive[slice(*after['Other.bffnt'])])
        font = bf.Bffnt(archive[slice(*before['External_00.bffnt'])])
        new_data = new_archive[slice(*after['External_00.bffnt'])]
        old_rows = bf.bc4_read(archive[slice(*before['External_00.bffnt'])][font.sheet_at:], font.texture)
        new_rows = bf.bc4_read(new_data[font.sheet_at:], font.texture)
        drawers = bp.drawers_of('xbox', letters)
        # Switch A (E040) now shows the drawing of Xbox B (position mode) on the disc of its own cell, and so on.
        for code, pad in ((0xE040, 'b'), (0xE041, 'a'), (0xE042, 'y'), (0xE043, 'x')):
            self.assertEqual(cells[code], bp.render(drawers[pad], 50, bp.disc_box(font.get(old_rows, code))))
            got = font.get(new_rows, code)
            self.assertLess(max(abs(x - y) for rw, rg in zip(cells[code], got) for x, y in zip(rw, rg)), 130)    # up to BC4's own rounding
            self.assertNotEqual(font.get(old_rows, code), got)

    @unittest.skipIf(mbi is None, 'needs fontTools (the Menu and View drawers come from make_button_icons.py)')
    def test_menu_and_view_discs_sit_where_the_games_disc_sits(self):
        # The game's + and - glyphs fill only the left part of their cell and the game draws only that part (a disc at the middle of
        # the cell was cut off): the new disc keeps the old one's place.
        old = [bytes(255 if (x - 15) ** 2 + (y - 22) ** 2 <= 13 ** 2 else 0 for x in range(50)) for y in range(50)]
        new, notes = bp.font_cells(StubFont({0xE04B: old, 0xE04C: old}), None, pl.prompt_controls('', 'position'), StubLetters())
        self.assertEqual(sorted(new), [0xE04B, 0xE04C])
        for code in new:
            xs = [x for row in new[code] for x, v in enumerate(row) if v > 128]
            ys = [y for y, row in enumerate(new[code]) for v in row if v > 128]
            self.assertLessEqual(abs((min(xs) + max(xs)) / 2 - 15), 2)
            self.assertLessEqual(abs((min(ys) + max(ys)) / 2 - 22), 2)
            self.assertLess(max(xs), 30)                                        # nothing in the half the game does not draw
        self.assertNotEqual(new[0xE04B], new[0xE04C])                           # Menu and View are two drawings
        new, notes = bp.font_cells(StubFont({0xE04B: [bytes(50)] * 50}), None, pl.prompt_controls('', 'position'), StubLetters())
        self.assertNotIn(0xE04B, new)                                           # negative control: a blank cell has no disc
        self.assertTrue(any(n.startswith('plus glyph') for n in notes))

    def test_unknown_style_is_refused(self):      # negative control
        data = bffnt(self.sheet())
        with self.assertRaises(ValueError):
            bp.make_font(yaz0_encode(sarc({'External_00.bffnt': data})), pl.prompt_controls('', 'position'), StubLetters(), 'nintendo')

    def odd_fonts(self):
        """name -> a BFFNT that differs from the game's in one way (the sheet's format, a second sheet, the kind of the CMAP,
        a glyph grid that does not fit the sheet, a cell code outside the grid)."""
        good = bffnt(self.sheet())
        sections = bf.Bffnt(good).sections
        tglp, cmap, brti = sections[b'TGLP'], good.rindex(b'CMAP'), good.index(b'BRTI', sections[b'TGLP'])
        out = {}
        for name, at, fmt, value in (('format', brti + 0x1c, '<I', bf.R8_UNORM), ('mips', brti + 0x16, '<H', 3),
                                     ('two sheets', tglp + 10, '<B', 2), ('cmap kind', cmap + 16, '<H', 2),
                                     ('grid', tglp + 8 + 12, '<H', 9), ('rows', tglp + 8 + 14, '<H', 30)):
            odd = bytearray(good); struct.pack_into(fmt, odd, at, value); out[name] = bytes(odd)
        return good, out

    def test_a_font_with_an_unexpected_sheet_or_no_external_font_is_refused(self):
        good, odd = self.odd_fonts()
        controls, letters = pl.prompt_controls('', 'position'), StubLetters()
        for name, data in odd.items():
            with self.subTest(name), self.assertRaises(ValueError):
                bp.make_font(yaz0_encode(sarc({'External_00.bffnt': data})), controls, letters)
        with self.assertRaises(bp.MissingData):
            bp.make_font(yaz0_encode(sarc({'Ancient_00.bffnt': good})), controls, letters)
        if mbi:        # negative control: the unchanged font is drawn
            self.assertTrue(bp.make_font(yaz0_encode(sarc({'External_00.bffnt': good})), controls, letters)[1])

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_playstation_font_redraws_the_face_glyphs_on_discs_like_the_originals(self):
        rows = [bytearray(256) for _ in range(4 * 51 + 4)]
        font = bf.Bffnt(bffnt(rows))
        for i, code in enumerate(sorted(font.cells)):          # a disc of a different size and place in each cell
            x0, y0, x1, y1 = font.cell_box(code)
            disc = disc_picture(25 + i, 25, 14 + i, 50)
            for y in range(50):
                rows[y0 + y][x0:x1] = disc[y]
        data = bffnt(rows)
        archive = sarc({'External_00.bffnt': data})
        controls = pl.prompt_controls('', 'position')
        _, xbox, _, _ = bp.make_font(yaz0_encode(archive), controls, StubLetters())
        new_stream, cells, _, _ = bp.make_font(yaz0_encode(archive), controls, StubLetters(), 'playstation')
        self.assertEqual(sorted(cells), [0xE040, 0xE041, 0xE042, 0xE043])
        font = bf.Bffnt(data)
        old_rows = bf.bc4_read(data[font.sheet_at:], font.texture)
        new_archive = bf.yaz0_decode(new_stream)
        new_data = new_archive[slice(*bf.sarc_members(new_archive)['External_00.bffnt'])]
        new_rows = bf.bc4_read(new_data[font.sheet_at:], font.texture)
        for code in cells:
            self.assertNotEqual(font.get(new_rows, code), font.get(old_rows, code))
            self.assertNotEqual(cells[code], xbox[code])
            same_disc(self, font.get(new_rows, code), font.get(old_rows, code))


def stick_glyph(size=50):
    """A stick press glyph like the game's: a white body (centre 17, 21.5, radius 16) with a grey ring on it, and a dark letter L
    on its cap that opens upwards out of the glyph, so no mark of it is enclosed."""
    rows = [bytearray(size) for _ in range(size)]
    for y in range(size):
        for x in range(size):
            d = ((x + 0.5 - 17) ** 2 + (y + 0.5 - 21.5) ** 2) ** 0.5
            rows[y][x] = 0 if d > 16 else 140 if 12.5 <= d <= 14 else 255
    for y in range(0, 26):          # the letter's upright stroke, open to the top edge
        rows[y][14] = rows[y][15] = 0
    for x in range(14, 21):
        rows[24][x] = rows[25][x] = 0
    return [bytes(r) for r in rows]


class StubFont:
    """The part of botw_formats.Bffnt that font_cells uses: cells by code point."""
    def __init__(self, cells, cell=50):
        self.cells, self.cell, self.cell_height = cells, cell, cell

    def get(self, sheet_rows, code):
        return self.cells[code]


@unittest.skipIf(bf is None, 'needs Python 3.14')
class StickGlyphs(unittest.TestCase):
    def test_the_stick_body_is_found_and_the_game_s_stick_is_traced_on_it(self):
        old = stick_glyph()
        self.assertEqual(bp.stick_body(old), (1, 5.5, 33, 37.5))              # the body's disc: 32 wide, on the rows that are wide enough
        if mbi is None:
            return
        controls = {**pl.prompt_controls('', 'position'), 'rstick': 'l3'}     # both presses on the left stick
        new, _ = bp.font_cells(StubFont({0xE056: old, 0xE057: [bytes(50)] * 50}), None, controls, StubLetters(), 'xbox', 2)
        self.assertEqual(new[0xE056], bp.traced(old, (1, 5.5, 33, 37.5), 100, (2, 11, 66, 75)))     # the game's stick, not an LS disc
        self.assertNotIn(0xE057, new)                                         # no stick in the right press glyph: it stays

    @unittest.skipIf(mbi is None, 'needs fontTools (the d-pad drawers come from make_button_icons.py)')
    def test_the_d_pad_cross_fills_the_box_of_the_dots_and_cuts_the_lit_arrows(self):
        dots = [bytes(255 if any((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= 36 for cx, cy in ((32, 14), (14, 32), (50, 32), (32, 50))) else 0
                      for x in range(64)) for y in range(64)]
        drawers = bp.drawers_of('xbox', StubLetters())
        box = bp.disc_box(dots)
        all_lit, up = bp.render(drawers[0xE049], 64, box), bp.render(drawers[0xE060], 64, box)
        self.assertEqual((all_lit[32][32], all_lit[32][10], all_lit[32][4], all_lit[10][10]), (255, 255, 0, 0))     # middle, arm's end, outside the box, a corner
        self.assertLess(all_lit[16][32], 64)                      # the up arrow is cut out of the top arm
        self.assertEqual((up[16][32] < 64, up[48][32]), (True, 255))   # only the up arrow when only up is lit
        with self.assertRaises(ValueError):
            bp.disc_box([bytes(64)] * 64)                        # negative control: no shape to cover

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_a_moving_stick_stays_the_game_s_and_is_traced_above_1x_in_every_style(self):
        cells = {0xE050: stick_glyph(), 0xE0A0: stick_glyph()}
        for style in ('xbox', 'playstation'):
            new, _ = bp.font_cells(StubFont(cells), None, pl.prompt_controls('', 'position'), StubLetters(), style)
            self.assertNotIn(0xE050, new, style)                                # at 1x the game's own
            new, _ = bp.font_cells(StubFont(cells), None, pl.prompt_controls('', 'position'), StubLetters(), style, 2)
            self.assertEqual(new[0xE050], bp.traced(cells[0xE050], (0, 0, 50, 50), 100, (0, 0, 100, 100)), style)
            self.assertNotIn(0xE0A0, new, style)                                # not a button glyph

    def test_a_cell_without_a_stick_is_refused(self):     # negative control
        with self.assertRaises(ValueError):
            bp.stick_body([bytes(50)] * 50)

    @unittest.skipIf(mbi is None, 'needs fontTools (the Menu and View drawers come from make_button_icons.py)')
    def test_font_cells_redraw_both_stick_glyphs_for_both_styles(self):
        disc = disc_picture(25, 22, 13, 50)
        cells = {code: disc for code in (*bp.FONT_FACE.values(), *bp.FONT_SHAPE.values(), *bp.FONT_MENU.values())}
        cells[0xE056] = cells[0xE057] = stick_glyph()
        for style in ('xbox', 'playstation'):
            new, notes = bp.font_cells(StubFont(cells), None, pl.prompt_controls('', 'position'), StubLetters(), style)
            self.assertFalse([n for n in notes if 'STICK' in n], style)
            drawers = bp.drawers_of(style, StubLetters())
            for code in (0xE056, 0xE057):                                     # the game's own stick, traced, in both styles
                self.assertEqual(new[code], bp.traced(cells[code], bp.stick_body(cells[code]), 50, bp.stick_body(cells[code])), (style, code))

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_switch_glyphs_are_the_games_own_traced_and_at_2x_every_button_glyph_is(self):
        cells = {code: disc_picture(25 + i % 3, 22, 13, 50) for i, code in enumerate((*bp.FONT_FACE.values(), *bp.FONT_MENU.values()))}
        cells.update({code: key_picture() + [bytes(48)] * 2 for code in (0xE044, 0xE045, 0xE046, 0xE047)})
        cells.update({0xE056: stick_glyph(), 0xE057: stick_glyph(), 0xE050: stick_glyph(), 0xE0A0: stick_glyph()})
        controls = {**pl.prompt_controls('', 'label'), 'a': 'x', 'rstick': 'l3'}
        new, notes = bp.font_cells(StubFont(cells), None, controls, StubLetters(), 'switch')
        self.assertEqual(new[0xE040], bp.traced(cells[0xE042], bp.disc_box(cells[0xE042]), 50, bp.disc_box(cells[0xE040])))   # A <- X
        self.assertEqual(new[0xE041], bp.traced(cells[0xE041], bp.disc_box(cells[0xE041]), 50, bp.disc_box(cells[0xE041])))
        self.assertEqual(new[0xE057], bp.traced(cells[0xE056], bp.stick_body(cells[0xE056]), 50, bp.stick_body(cells[0xE057])))
        self.assertEqual(new[0xE04B], bp.traced(cells[0xE04B], bp.disc_box(cells[0xE04B]), 50, bp.disc_box(cells[0xE04B])))
        self.assertNotIn(0xE050, new)                                   # a stick being moved: at 1x the game's own
        new, _ = bp.font_cells(StubFont(cells), None, controls, StubLetters(), 'switch', 2)
        self.assertEqual(new[0xE050], bp.traced(cells[0xE050], (0, 0, 50, 50), 100, (0, 0, 100, 100)))
        self.assertNotIn(0xE0A0, new)                                   # not a button glyph: enlarged as the sheet is

    @unittest.skipIf(mbi is None, 'needs fontTools (the Menu and View drawers come from make_button_icons.py)')
    def test_the_d_pad_glyphs_become_a_cross_in_the_place_of_the_four_dots(self):
        dots = [bytearray(50) for _ in range(50)]
        for cx, cy in ((16, 12), (8, 20), (24, 20), (16, 28)):       # four dots round (16, 20), in the left part of the cell
            for y in range(50):
                for x in range(50):
                    if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= 16:
                        dots[y][x] = 255
        dots = [bytes(r) for r in dots]
        disc = disc_picture(25, 22, 13, 50)
        cells = {code: disc for code in (*bp.FONT_FACE.values(), *bp.FONT_SHAPE.values(), *bp.FONT_MENU.values())}
        cells.update({0xE056: stick_glyph(), 0xE057: stick_glyph(), 0xE049: dots, 0xE060: dots})
        new, notes = bp.font_cells(StubFont(cells), None, pl.prompt_controls('', 'position'), StubLetters())
        self.assertTrue({0xE049, 0xE060} <= set(new))
        self.assertFalse([n for n in notes if 'd-pad' in n])
        self.assertEqual(new[0xE049][20][16], 255)                  # the middle of the cross is filled (between the dots it was empty)
        self.assertNotEqual(new[0xE049], new[0xE060])               # E060 lights the up arrow
        xs = [x for row in new[0xE049] for x, v in enumerate(row) if v > 128]
        self.assertLess(max(xs), 30)                                 # it stays in the place of the dots


@unittest.skipIf(bf is None, 'needs Python 3.14')
class SystemFont(unittest.TestCase):
    def test_bfttf_decodes_to_the_font_inside(self):
        font = b'\x00\x01\x00\x00' + bytes(range(30))       # a TrueType header and some bytes (30 + 4 is not a multiple of 4)
        key = 0x12345678
        words = font[:len(font) - len(font) % 4]
        blob = struct.pack('>II', bp.BFTTF_MAGIC ^ key, len(font) ^ key)
        blob += b''.join(struct.pack('>I', struct.unpack('>I', words[i:i + 4])[0] ^ key) for i in range(0, len(words), 4)) + font[len(words):]
        self.assertEqual(bp.bfttf_decode(blob), font)


def expected_texture(letters, style, pad, old, scale=1, shape='disc'):
    """What the texture of the button `pad` must be for the game's picture `old`: the drawer of the button, fitted on the white
    shape of `old` (its disc, or its bright box for a key), `scale` times as large."""
    box = bp.disc_box(old) if shape == 'disc' else bp.bright_box(old)
    return bp.render(bp.drawers_of(style, letters)[pad], len(old) * scale, bp.enlarged(box, scale))


def expected_stick(old, scale=1):
    """What a stick press texture must be in every style: the game's own picture `old`, traced on its bright box."""
    box = bp.bright_box(old)
    return bp.traced(old, box, len(old) * scale, bp.enlarged(box, scale))


@unittest.skipIf(bf is None or mbi is None, 'needs Python 3.14 and fontTools')
class MakePack(unittest.TestCase):
    def setUp(self):
        self.pics = {n: picture(i) for i, n in enumerate(NAMES)}
        for n in NAMES[4:]:
            self.pics[n] = key_picture()
        self.layout = sarc({'blyt/other.bflyt': sample(3000, 5), 'timg/__Combined.bntx': bntx(self.pics), 'anim/x.bflan': sample(500, 6)})
        self.pack = sarc({'Pack/other': sample(2000, 7), 'Layout/Common.sblarc': yaz0_encode(self.layout), 'Pack/tail': sample(900, 8)})

    def pictures(self, pack):
        layout = bf.yaz0_decode(pack[slice(*bf.sarc_members(pack)['Layout/Common.sblarc'])])
        inner = bf.sarc_members(layout); blob = layout[slice(*inner['timg/__Combined.bntx'])]
        return {n: bf.r8_read(blob, t) for n, t in bf.bntx_textures(blob).items()}

    def test_default_layout_draws_the_buttons_that_are_pressed_on_the_games_shapes(self):
        letters = StubLetters()
        new_pack, changed, _, notes, _ = bp.make_pack(self.pack, pl.prompt_controls('', 'position'), letters)
        got = self.pictures(new_pack)
        for name, pad in zip(NAMES[:4], 'bayx'):          # Switch A shows the Xbox B drawing (position mode), and so on
            self.assertEqual(got[name], expected_texture(letters, 'xbox', pad, self.pics[name]))
        for name, pad in zip(NAMES[4:6], ('l1', 'l2')):
            self.assertEqual(got[name], expected_texture(letters, 'xbox', pad, self.pics[name], shape='key'))
            self.assertNotEqual(got[name], self.pics[name])
        for name in NAMES[6:]:                              # the stick press: the game's own stick, traced
            self.assertEqual(got[name], expected_stick(self.pics[name]))
        self.assertEqual(sorted(changed), sorted(NAMES))

    def test_owner_layout_by_label(self):
        owner = 'north = a\nsouth = x\nwest = y\neast = b\n'
        new_pack, _, _, _, _ = bp.make_pack(self.pack, pl.prompt_controls(owner, 'label'), StubLetters())
        got = self.pictures(new_pack)
        letters = StubLetters()
        for name, pad in zip(NAMES[:4], 'xaby'):        # A <- X, B <- A, X <- B, Y <- Y
            self.assertEqual(got[name], expected_texture(letters, 'xbox', pad, self.pics[name]))

    def test_everything_else_is_the_original(self):
        new_pack, _, _, _, sizes = bp.make_pack(self.pack, pl.prompt_controls('', 'position'), StubLetters())
        before, after = bf.sarc_members(self.pack), bf.sarc_members(new_pack)
        self.assertEqual(before.keys(), after.keys())
        for name in before:
            if name != 'Layout/Common.sblarc':
                self.assertEqual(self.pack[slice(*before[name])], new_pack[slice(*after[name])], name)
        self.assertEqual(sizes[2], len(self.layout))              # the decompressed size is unchanged
        self.assertEqual(sizes[3], 0)
        old = bf.yaz0_decode(self.pack[slice(*before['Layout/Common.sblarc'])])
        new = bf.yaz0_decode(new_pack[slice(*after['Layout/Common.sblarc'])])
        a, b = bf.sarc_members(old), bf.sarc_members(new)
        for name in a:
            if name != 'timg/__Combined.bntx':
                self.assertEqual(old[slice(*a[name])], new[slice(*b[name])], name)

    def test_a_button_nothing_drives_keeps_its_picture(self):
        controls = pl.prompt_controls('menu = none\nl1 = none\nl2 = none\n', 'position')
        controls['lstick'] = None
        new_pack, changed, _, notes, _ = bp.make_pack(self.pack, controls, StubLetters())
        self.assertNotIn(NAMES[4], changed)       # nothing drives L, so its picture stays
        self.assertTrue(any(n.startswith('L:') for n in notes))

    def test_a_texture_in_another_format_is_skipped_with_a_note(self):
        bad = sarc({'timg/__Combined.bntx': bntx({n: picture(1) for n in NAMES}, fmt=0x1d01)})
        pack = sarc({'Layout/Common.sblarc': yaz0_encode(bad)})
        new_pack, changed, _, notes, _ = bp.make_pack(pack, pl.prompt_controls('', 'position'), StubLetters())
        self.assertEqual(changed, {})
        self.assertEqual(new_pack, pack)                         # nothing is written into the archive
        for name in NAMES:
            self.assertTrue(any(n.startswith(name + ':') and 'format' in n for n in notes), name)
        _, changed, _, _, _ = bp.make_pack(self.pack, pl.prompt_controls('', 'position'), StubLetters())      # negative control: the right format is drawn
        self.assertEqual(sorted(changed), sorted(NAMES))

    def test_missing_wrong_sized_and_multi_mip_textures_are_skipped_the_others_are_drawn(self):
        from test_botw_grow import full_bntx
        names = NAMES + [bp.DPAD_TEXTURE]
        pics = {n: self.pics[n] for n in NAMES}
        pics[bp.DPAD_TEXTURE] = disc_picture(32, 32, 24, 64)
        for case in ('missing', 'size', 'mips', 'dpad size'):
            specs = {n: (bf.R8_UNORM, len(rows), len(rows), *bf.r8_pack(rows)) for n, rows in pics.items()}
            bad = {'missing': NAMES[0], 'size': NAMES[1], 'mips': NAMES[2], 'dpad size': bp.DPAD_TEXTURE}[case]
            if case == 'missing':
                del specs[bad]
            elif case == 'size':
                rows = disc_picture(32, 32, 24, 64); specs[bad] = (bf.R8_UNORM, 64, 64, *bf.r8_pack(rows))
            elif case == 'dpad size':
                rows = disc_picture(24, 24, 20, 48); specs[bad] = (bf.R8_UNORM, 48, 48, *bf.r8_pack(rows))
            blob = bytearray(full_bntx(specs))
            if case == 'mips':
                brti = next(m.start() for m in re.finditer(b'BRTI', blob) if bf.Texture(blob, m.start()).name == bad)
                struct.pack_into('<H', blob, brti + 0x16, 2)
            pack = sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/__Combined.bntx': bytes(blob)}))})
            for scale in (1, 2):
                new_pack, changed, _, notes, _ = bp.make_pack(pack, pl.prompt_controls('', 'position'), StubLetters(), 'xbox', True, scale)
                with self.subTest(case=case, scale=scale):
                    self.assertNotIn(bad, changed)
                    self.assertEqual(sorted(changed), sorted(n for n in names if n != bad))      # every other texture is drawn
                    self.assertEqual(len([n for n in notes if n.startswith(bad + ':')]), 1, notes)
                    layout = bf.yaz0_decode(new_pack[slice(*bf.sarc_members(new_pack)['Layout/Common.sblarc'])])
                    inner = bf.sarc_members(layout)
                    textures = bf.bntx_textures(layout[slice(*inner['timg/__Combined.bntx'])])
                    if bad in textures:       # the skipped texture keeps its size
                        self.assertEqual((textures[bad].width, textures[bad].height), (specs[bad][1], specs[bad][2]))
                    for good in set(names) - {bad}:
                        self.assertEqual(textures[good].width, len(changed[good]))

    def test_controller_pictures_in_another_format_or_size_are_skipped_with_a_note(self):
        from test_botw_grow import full_bntx
        name = 'Nt_Picture_00^f'
        specs = {n: (bf.R8_UNORM, 48, 48, *bf.r8_pack(self.pics[n])) for n in NAMES}
        draws = []

        def draw(controls, letters, text, style='xbox', scale=1):
            draws.append(name)
            return [bytes([60]) * 64] * 32, [bytes([200]) * 64] * 32
        old = bp.PICTURES
        try:
            bp.PICTURES = {name: ((64, 32), draw)}
            for case, spec in (('format', (bf.R8_UNORM, 64, 32, *bf.r8_pack([bytes(64)] * 32))),
                               ('size', (bf.R8G8_UNORM, 32, 32, *bf.rg8_pack([bytes(32)] * 32, [bytes(32)] * 32))),
                               ('missing', None),
                               ('right', (bf.R8G8_UNORM, 64, 32, *bf.rg8_pack([bytes(64)] * 32, [bytes(64)] * 32)))):     # negative control
                pack = sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/__Combined.bntx': full_bntx({**specs, **({name: spec} if spec else {})})}))})
                _, _, drawn, notes, _ = bp.make_pack(pack, pl.prompt_controls('', 'position'), StubLetters())
                with self.subTest(case=case):
                    self.assertEqual(sorted(drawn), [name] if case == 'right' else [])
                    self.assertEqual(any(n.startswith(name) for n in notes), case != 'right', notes)
        finally:
            bp.PICTURES = old

    def test_a_hint_layout_that_cannot_be_read_keeps_the_rings_with_a_note(self):
        controls = pl.prompt_controls('', 'position')
        for blob in (b'FLYT', b'not a layout at all', b'FLYT' + bytes(60)):
            edits, notes = bp.ring_edits(blob, {bp.HINT: (0, len(blob))}, controls)
            self.assertEqual((edits, len(notes)), ([], 1), blob)
        edits, notes = bp.ring_edits(b'', {}, controls)            # no hint layout in the archive
        self.assertEqual((edits, len(notes)), ([], 1))

    def test_an_archive_without_the_layout_or_its_bntx_is_refused_clearly(self):
        controls = pl.prompt_controls('', 'position')
        with self.assertRaises(bp.MissingData):
            bp.make_pack(sarc({'Pack/other': sample(100, 1)}), controls, StubLetters())
        with self.assertRaises(bp.MissingData):
            bp.make_pack(sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/other.bntx': bntx(self.pics)}))}), controls, StubLetters())


@unittest.skipIf(bf is None or mbi is None, 'needs Python 3.14 and fontTools')
class MakePackPlayStation(unittest.TestCase):
    def setUp(self):
        self.pics = {n: disc_picture(24 + i * 0.5, 23 + i * 0.5, 17 + i * 0.5) for i, n in enumerate(NAMES[:4])}
        for n in NAMES[4:]:
            self.pics[n] = key_picture()
        self.layout = sarc({'blyt/other.bflyt': sample(3000, 5), 'timg/__Combined.bntx': bntx(self.pics), 'anim/x.bflan': sample(500, 6)})
        self.pack = sarc({'Pack/other': sample(2000, 7), 'Layout/Common.sblarc': yaz0_encode(self.layout), 'Pack/tail': sample(900, 8)})

    def pictures(self, pack):
        layout = bf.yaz0_decode(pack[slice(*bf.sarc_members(pack)['Layout/Common.sblarc'])])
        inner = bf.sarc_members(layout); blob = layout[slice(*inner['timg/__Combined.bntx'])]
        return {n: bf.r8_read(blob, t) for n, t in bf.bntx_textures(blob).items()}

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_face_textures_are_drawn_on_discs_like_the_originals_and_the_rest_is_the_original(self):
        controls = pl.prompt_controls('', 'position')
        xbox_pack = bp.make_pack(self.pack, controls, StubLetters())[0]
        new_pack, changed, _, notes, sizes = bp.make_pack(self.pack, controls, StubLetters(), 'playstation')
        got, xbox = self.pictures(new_pack), self.pictures(xbox_pack)
        for name in NAMES[:4]:
            self.assertNotEqual(got[name], self.pics[name])
            self.assertNotEqual(got[name], xbox[name])
            same_disc(self, got[name], self.pics[name])
        for name, pad in zip(NAMES[4:6], ('l1', 'l2')):
            self.assertEqual(got[name], expected_texture(StubLetters(), 'playstation', pad, self.pics[name], shape='key'))
        for name in NAMES[6:]:
            self.assertEqual(got[name], expected_stick(self.pics[name]))
        self.assertEqual(sorted(changed), sorted(NAMES))
        self.assertFalse([n for n in notes if 'stay the Switch' in n])     # PlayStation has its own controller pictures now
        self.assertTrue(all('not found' in n for n in notes))               # this made-up layout holds none of them
        before, after = bf.sarc_members(self.pack), bf.sarc_members(new_pack)
        for name in before:
            if name != 'Layout/Common.sblarc':
                self.assertEqual(self.pack[slice(*before[name])], new_pack[slice(*after[name])], name)
        self.assertEqual(sizes[2], len(self.layout))
        old = bf.yaz0_decode(self.pack[slice(*before['Layout/Common.sblarc'])])
        new = bf.yaz0_decode(new_pack[slice(*after['Layout/Common.sblarc'])])
        a, b = bf.sarc_members(old), bf.sarc_members(new)
        for name in a:
            if name != 'timg/__Combined.bntx':
                self.assertEqual(old[slice(*a[name])], new[slice(*b[name])], name)

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_a_face_button_without_a_playstation_picture_keeps_its_texture(self):
        controls = {**pl.prompt_controls('', 'position'), 'a': 'l1'}      # L1 has no face picture
        new_pack, changed, _, notes, _ = bp.make_pack(self.pack, controls, StubLetters(), 'playstation')
        self.assertNotIn(NAMES[0], changed)
        self.assertEqual(self.pictures(new_pack)[NAMES[0]], self.pics[NAMES[0]])
        self.assertTrue(any(n.startswith('A:') and 'PlayStation' not in n and 'face picture' in n for n in notes))

    def test_unknown_style_is_refused(self):      # negative control
        with self.assertRaises(ValueError):
            bp.make_pack(self.pack, pl.prompt_controls('', 'position'), StubLetters(), 'nintendo')


@unittest.skipIf(bf is None or mbi is None, 'needs Python 3.14 and fontTools')
class MakePackSwitch(unittest.TestCase):
    """--style switch: the game's own pictures, traced (sharp at a larger scale), of the button pressed (controls.txt)."""
    setUp, pictures = MakePack.setUp, MakePack.pictures

    def expected(self, source, target, find, scale=1):
        return bp.traced(self.pics[source], find(self.pics[source]), 48 * scale, bp.enlarged(find(self.pics[target]), scale))

    def test_default_layout_by_label_traces_each_switch_button_from_itself(self):
        new_pack, changed, drawn, _, _ = bp.make_pack(self.pack, pl.prompt_controls('', 'label'), StubLetters(), 'switch')
        got = self.pictures(new_pack)
        for name in NAMES[:4]:
            self.assertEqual(got[name], self.expected(name, name, bp.disc_box))
        for name in NAMES[4:]:
            self.assertEqual(got[name], self.expected(name, name, bp.bright_box))
        self.assertEqual(sorted(changed), sorted(NAMES))
        self.assertEqual(drawn, {})                       # the controller pictures stay the Switch's

    def test_a_remap_traces_the_picture_of_the_button_pressed(self):
        owner = 'north = a\nsouth = x\nwest = y\neast = b\nl1 = zl\nl2 = l\n'
        new_pack, *_ = bp.make_pack(self.pack, pl.prompt_controls(owner, 'label'), StubLetters(), 'switch')
        got = self.pictures(new_pack)
        for name, source in zip(NAMES[:4], (NAMES[2], NAMES[0], NAMES[1], NAMES[3])):       # A <- X, B <- A, X <- B, Y <- Y
            self.assertEqual(got[name], self.expected(source, name, bp.disc_box), name)
        self.assertEqual(got[NAMES[4]], self.expected(NAMES[5], NAMES[4], bp.bright_box))     # L <- ZL's key
        self.assertEqual(got[NAMES[5]], self.expected(NAMES[4], NAMES[5], bp.bright_box))     # ZL <- L's key

    def test_a_button_with_no_texture_is_drawn(self):
        controls = {**pl.prompt_controls('', 'label'), 'l': 'r1'}          # R has no texture: the drawn R key stands in
        new_pack, *_ = bp.make_pack(self.pack, controls, StubLetters(), 'switch')
        self.assertEqual(self.pictures(new_pack)[NAMES[4]], expected_texture(StubLetters(), 'switch', 'r1', self.pics[NAMES[4]], shape='key'))

    def test_the_drawn_stand_ins_carry_the_switch_names(self):
        switch, xbox = bp.drawers_of('switch', StubLetters()), bp.drawers_of('xbox', StubLetters())
        same = [pad for pad in ('a', 'b', 'x', 'y', 'l1', 'menu', 'view', 'l3', 0xE049)        # (ZL and LT: two stub letters each)
                if bp.render(switch[pad], 48, (4, 4, 44, 44)) == bp.render(xbox[pad], 48, (4, 4, 44, 44))]
        self.assertEqual(same, ['a', 'b', 'x', 'y', 'l3', 0xE049])      # a stick press stands in as the Switch's in every style
        self.assertNotEqual(bp.render(switch['menu'], 48, (4, 4, 44, 44)), bp.render(switch['view'], 48, (4, 4, 44, 44)))   # + and -
        self.assertEqual(bp.style_parts('switch'), (bp.SWITCH_TEXT, 'Switch'))


@unittest.skipIf(bf is None, 'needs Python 3.14')
class Traced(unittest.TestCase):
    """bp.traced: the game's picture drawn again larger, with sharp edges where its anti-aliasing puts them."""
    def ring(self, size=48):
        """A white disc with a grey ring (150) on it, anti-aliased (4x4 samples per pixel), on a dark (5) ground."""
        rows = []
        for y in range(size):
            row = bytearray()
            for x in range(size):
                total = 0
                for i in range(4):
                    for j in range(4):
                        d = ((x + (i + 0.5) / 4 - 24) ** 2 + (y + (j + 0.5) / 4 - 24) ** 2) ** 0.5
                        total += 5 if d > 18 else 150 if 11 < d < 14 else 255
                row.append(round(total / 16))
            rows.append(bytes(row))
        return rows

    def test_the_flat_levels_of_a_picture(self):
        self.assertEqual(bp.levels(self.ring()), [5, 150, 255])
        self.assertEqual(bp.levels([bytes(10)] * 10), [0])

    def test_twice_as_large_shrinks_back_to_the_picture_and_is_sharper_than_repeated_pixels(self):
        old = self.ring()
        new = bp.traced(old, (0, 0, 48, 48), 96, (0, 0, 96, 96))
        small = downsampled(new, 2)
        self.assertLess(max(abs(a - b) for r, q in zip(small, old) for a, b in zip(r, q)), 70)
        self.assertLess(sum(abs(a - b) for r, q in zip(small, old) for a, b in zip(r, q)) / 48 ** 2, 4)
        def blurred(rows):      # pixels between the levels: the soft edge
            return sum(1 for r in rows for v in r if all(abs(v - level) > 20 for level in (5, 150, 255)))
        nearest = [bytes(v for v in r for _ in range(2)) for r in old for _ in range(2)]
        self.assertLess(blurred(new), blurred(nearest))
        self.assertEqual(new[48][48], 255)                          # the middle stays white, the ring grey, the ground dark
        self.assertEqual(new[48][48 + 25], 150)
        self.assertEqual(new[2][2], 5)

    def test_the_part_in_the_box_is_fitted_on_the_target(self):
        old = self.ring()
        new = bp.traced(old, (6, 6, 42, 42), 48, (12, 12, 36, 36))      # the disc made half as large, in the middle
        xs = [x for r in new for x, v in enumerate(r) if v > 128]
        self.assertEqual((min(xs), max(xs)), (12, 35))


def downsampled(rows, k):
    """`rows` shrunk k times by the mean of each k x k block."""
    return [[sum(rows[y * k + j][x * k + i] for j in range(k) for i in range(k)) / (k * k) for x in range(len(rows[0]) // k)]
            for y in range(len(rows) // k)]


@unittest.skipIf(bf is None or mbi is None, 'needs Python 3.14 and fontTools')
class Scale(unittest.TestCase):
    """--scale above 1: new larger textures (full BNTX of the game's layout) and a larger glyph sheet, one drawing for both."""
    SIZE = 48       # the face textures are as large as the font's cells here, so one button has one picture in both

    def setUp(self):
        from test_botw_grow import full_bntx, font
        self.letters, self.controls = StubLetters(), pl.prompt_controls('', 'position')
        self.pics = {n: disc_picture(24 + i * 0.5, 23 + i * 0.5, 17 + i * 0.5, self.SIZE) for i, n in enumerate(NAMES[:4])}
        self.pics.update({n: key_picture() for n in NAMES[4:]})
        self.pics['Nt_Other_00^d'] = picture(77)                     # a texture nobody changes
        specs = {n: (bf.R8_UNORM, len(rows), len(rows), *bf.r8_pack(rows)) for n, rows in self.pics.items()}
        self.layout = sarc({'blyt/other.bflyt': sample(3000, 5), 'timg/__Combined.bntx': full_bntx(specs), 'anim/x.bflan': sample(500, 6)})
        self.pack = sarc({'Pack/other': sample(2000, 7), 'Layout/Common.sblarc': yaz0_encode(self.layout), 'Pack/tail': sample(900, 8)})
        self.sheet = [bytearray(((x // 4) * 9 + (y // 4) * 5) % 256 for x in range(256)) for y in range(208)]
        self.font_archive = sarc({'Ancient_00.bffnt': sample(3000, 5), 'External_00.bffnt': font(self.sheet), 'z.bin': sample(5000, 6)})
        self.font_stream = yaz0_encode(self.font_archive)

    def textures(self, pack):
        layout = bf.yaz0_decode(pack[slice(*bf.sarc_members(pack)['Layout/Common.sblarc'])])
        inner = bf.sarc_members(layout); blob = layout[slice(*inner['timg/__Combined.bntx'])]
        return layout, inner, blob, bf.bntx_textures(blob)

    def test_textures_are_new_and_larger_and_the_rest_is_the_original(self):
        new_pack, changed, drawn, notes, sizes = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', True, 2)
        layout, inner, blob, textures = self.textures(new_pack)
        old_layout, old_inner, old_blob, old_textures = self.textures(self.pack)
        for name in NAMES:
            self.assertEqual((textures[name].width, textures[name].height), (2 * old_textures[name].width, 2 * old_textures[name].height))
            self.assertEqual(bf.r8_read(blob, textures[name]), changed[name])
        for name, pad in zip(NAMES[:4], 'bayx'):
            self.assertEqual(changed[name], expected_texture(self.letters, 'xbox', pad, self.pics[name], 2))
        for name, pad in zip(NAMES[4:6], ('l1', 'l2')):
            self.assertEqual(changed[name], expected_texture(self.letters, 'xbox', pad, self.pics[name], 2, 'key'))
        for name in NAMES[6:]:
            self.assertEqual(changed[name], expected_stick(self.pics[name], 2))
        other = 'Nt_Other_00^d'
        self.assertEqual((textures[other].width, textures[other].height), (48, 48))
        self.assertEqual(bf.r8_read(blob, textures[other]), bf.r8_read(old_blob, old_textures[other]))     # an unchanged texture
        before, after = bf.sarc_members(self.pack), bf.sarc_members(new_pack)
        for name in before:
            if name != 'Layout/Common.sblarc':
                self.assertEqual(self.pack[slice(*before[name])], new_pack[slice(*after[name])], name)       # other members: byte for byte
        for name in old_inner:
            if name != 'timg/__Combined.bntx':
                self.assertEqual(old_layout[slice(*old_inner[name])], layout[slice(*inner[name])], name)
        self.assertEqual(sizes[2], len(self.layout))                    # the growth of the decompressed layout archive is reported
        self.assertEqual(sizes[3], len(layout) - len(self.layout))
        self.assertGreater(sizes[3], 0)
        self.assertEqual(sizes[3] % bf.GROW_ALIGN, 0)

    def test_switch_textures_are_traced_larger_too(self):
        controls = pl.prompt_controls('', 'label')
        new_pack, changed, drawn, _, sizes = bp.make_pack(self.pack, controls, self.letters, 'switch', True, 2)
        _, _, blob, textures = self.textures(new_pack)
        for name in NAMES[:4]:
            box = bp.disc_box(self.pics[name])
            self.assertEqual(changed[name], bp.traced(self.pics[name], box, 96, bp.enlarged(box, 2)))
            self.assertEqual(bf.r8_read(blob, textures[name]), changed[name])
        self.assertEqual(drawn, {})
        self.assertGreater(sizes[3], 0)

    def test_scale_1_writes_in_place(self):
        new_pack, changed, _, _, sizes = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', True, 1)
        self.assertEqual((sizes[2], sizes[3]), (len(self.layout), 0))
        layout, _, blob, textures = self.textures(new_pack)
        self.assertEqual(len(layout), len(self.layout))
        self.assertEqual(bf.r8_read(blob, textures[NAMES[0]]), changed[NAMES[0]])

    def test_a_scale_2_texture_is_the_scale_1_drawing_finer(self):
        _, one, _, _, _ = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', False, 1)
        _, two, _, _, _ = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', False, 2)
        for name in NAMES:
            small = downsampled(two[name], 2)
            self.assertLess(max(abs(a - b) for r, q in zip(small, one[name]) for a, b in zip(r, q)), 100, name)

    def test_a_texture_in_another_format_is_skipped_at_scale_2(self):
        bad = sarc({'timg/__Combined.bntx': bntx({n: picture(1) for n in NAMES}, fmt=0x1d01)})
        pack = sarc({'Layout/Common.sblarc': yaz0_encode(bad)})
        new_pack, changed, _, notes, sizes = bp.make_pack(pack, self.controls, self.letters, 'xbox', True, 2)
        self.assertEqual((changed, new_pack, sizes[3]), ({}, pack, 0))         # nothing grows
        self.assertEqual(len([n for n in notes if 'format' in n]), len(NAMES))

    def test_pictures_come_from_the_art_at_the_same_scale(self):
        from test_botw_grow import full_bntx
        calls = []

        def draw(controls, letters, text, style='xbox', scale=1):
            calls.append(scale)
            return [bytes([60]) * 64 * scale] * 32 * scale, [bytes([200]) * 64 * scale] * 32 * scale
        name = 'Nt_Picture_00^f'
        specs = {n: (bf.R8_UNORM, len(rows), len(rows), *bf.r8_pack(rows)) for n, rows in self.pics.items() if n in NAMES}
        specs[name] = (bf.R8G8_UNORM, 64, 32, *bf.rg8_pack([bytes(64)] * 32, [bytes(64)] * 32))
        pack = sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/__Combined.bntx': full_bntx(specs)}))})
        old, bp.PICTURES = bp.PICTURES, {name: ((64, 32), draw)}
        try:
            new_pack, _, drawn, _, _ = bp.make_pack(pack, self.controls, self.letters, 'xbox', True, 2)
        finally:
            bp.PICTURES = old
        self.assertEqual(calls, [2])
        _, _, blob, textures = self.textures(new_pack)
        self.assertEqual((textures[name].width, textures[name].height), (128, 64))
        self.assertEqual(bf.rg8_read(blob, textures[name]), drawn[name])

    def test_a_button_has_one_drawing_in_its_texture_and_in_its_font_glyph(self):
        _, textures, _, _, _ = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', False, 2)
        # the font cell of Switch A is a disc of the texture's size and place (both are 48 pixel pictures here)
        cells, _ = bp.font_cells(StubFont({0xE040: self.pics[NAMES[0]]}, cell=self.SIZE), None, self.controls, self.letters, 'xbox', 2)
        self.assertEqual(len(cells[0xE040]), 2 * self.SIZE)
        self.assertEqual(cells[0xE040], textures[NAMES[0]])

    def test_a_font_whose_cells_are_not_square_is_redrawn_at_1x_and_2x(self):
        # 1.6.0's External_00.bffnt: 44 x 39 cells in a 2 x 25 grid on a 128 x 1024 sheet (1.9.0: 50 x 50, 5 x 20 on 256 x 1024)
        from test_botw_grow import font
        sheet = [bytearray(((x // 4) * 9 + (y // 4) * 5) % 256 for x in range(128)) for y in range(1024)]
        codes = (0xE040, 0xE041, 0xE042, 0xE043)
        data = font(sheet, codes, (44, 39), (2, 25))
        stream = yaz0_encode(sarc({'External_00.bffnt': data}))
        old = bf.Bffnt(data)
        self.assertEqual((old.cell, old.cell_height), (44, 39))
        self.assertEqual(old.cell_box(0xE040), (1, 1024 - 40, 45, 1024 - 1))          # glyph 0: top left upright (the sheet is stored
        self.assertEqual(old.cell_box(0xE043), (46, 1024 - 80, 90, 1024 - 41))        # upside down), behind a one-pixel gap
        for scale in (1, 2):
            new_stream, cells, _, _ = bp.make_font(stream, self.controls, self.letters, 'xbox', scale)
            self.assertEqual(sorted(cells), list(codes))
            self.assertTrue(all((len(c), len(c[0])) == (39 * scale, 44 * scale) for c in cells.values()))
            out = bf.yaz0_decode(new_stream)
            new = bf.Bffnt(out[slice(*bf.sarc_members(out)['External_00.bffnt'])])
            self.assertEqual((new.texture.width, new.texture.height, new.cell, new.cell_height), (128 * scale, 1024 * scale, 44, 39))

    def test_the_font_sheet_grows_unchanged_cells_are_repeated_and_the_rest_is_the_original(self):
        new_stream, cells, _, grown = bp.make_font(self.font_stream, self.controls, self.letters, 'xbox', 2)
        archive = bf.yaz0_decode(self.font_stream)
        out = bf.yaz0_decode(new_stream)
        self.assertEqual(grown, len(out) - len(archive))
        self.assertGreater(grown, 0)
        self.assertEqual(grown % bf.GROW_ALIGN, 0)
        before, after = bf.sarc_members(archive), bf.sarc_members(out)
        for name in ('Ancient_00.bffnt', 'z.bin'):
            self.assertEqual(archive[slice(*before[name])], out[slice(*after[name])], name)
        old = bf.Bffnt(archive[slice(*before['External_00.bffnt'])])
        data = out[slice(*after['External_00.bffnt'])]
        new = bf.Bffnt(data)
        self.assertEqual((new.texture.width, new.texture.height, new.cell, new.columns, new.rows), (512, 416, old.cell, old.columns, old.rows))
        self.assertEqual(sorted(cells), [0xE040, 0xE041])
        big = bf.bc4_read(data[new.sheet_at:], new.texture)
        # outside the two redrawn cells the sheet is the old one with every pixel repeated 2 x 2 (this sheet is flat in 4 pixel blocks)
        redrawn = [tuple(2 * v for v in old.cell_box(code)) for code in cells]
        for y in range(0, 416, 7):
            for x in range(0, 512, 5):
                if not any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in redrawn):
                    self.assertEqual(big[y][x], self.sheet[y // 2][x // 2], (x, y))
        for code in cells:      # a redrawn cell is the drawing at twice the size, up to BC4's own rounding
            x0, y0, x1, y1 = (2 * v for v in old.cell_box(code))       # the TGLP's grid stays: the cell's place doubles
            got = [bytes(r[x0:x1]) for r in reversed(big[y0:y1])]
            self.assertEqual((len(got), len(got[0])), (100, 100))
            self.assertLess(sum(abs(a - b) for r, q in zip(got, cells[code]) for a, b in zip(r, q)) / 10000, 40)

    def romfs_with_everything(self, tmp, files=None, names=('Layout/Common.blarc', 'Font/Font_US.bfarc', 'Other.bin')):
        """A RomFS folder (made up) holding the pack, a font archive and a resource size table of `names`, and the firmware's
        system font. `files` replaces files of the RomFS by name (None leaves one out)."""
        import zlib
        entries = sorted((zlib.crc32(n.encode()), 1000 * (i + 1)) for i, n in enumerate(names))
        table = b'RSTB' + struct.pack('<II', len(entries), 0) + b''.join(struct.pack('<II', *e) for e in entries)
        files = {bp.PACK: self.pack, 'Font/Font_US.sbfarc': self.font_stream, bf.RSTB: yaz0_encode(table), **(files or {})}
        files = {name: data for name, data in files.items() if data is not None}
        index, blob = {}, b''
        for name, data in files.items():
            index[name] = (len(blob), len(data)); blob += data
        romfs = tmp / 'game' / 'botw190' / 'romfs'
        romfs.mkdir(parents=True)
        (romfs / 'romfs.bin').write_bytes(blob)
        (romfs / 'runtime-index.bin').write_bytes(runtime_index(index))
        buffer = io.BytesIO(); stub_font().save(buffer); ttf, key = buffer.getvalue(), 0x12345678
        words = ttf[:len(ttf) - len(ttf) % 4]
        (tmp / 'game' / 'firmware' / bp.FONT).parent.mkdir(parents=True)
        (tmp / 'game' / 'firmware' / bp.FONT).write_bytes(
            struct.pack('>II', bp.BFTTF_MAGIC ^ key, len(ttf) ^ key)
            + b''.join(struct.pack('>I', struct.unpack('>I', words[i:i + 4])[0] ^ key) for i in range(0, len(words), 4)) + ttf[len(words):])
        return romfs, entries

    def main_with(self, mod, romfs, *args):
        import contextlib
        argv = sys.argv
        sys.argv = ['botw_prompts.py', str(mod), '--romfs', str(romfs), *args]
        try:
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                bp.main()
        finally:
            sys.argv = argv

    def run_tool(self, mod, romfs, *args):
        """(exit code, everything printed) of a run of the tool."""
        import contextlib
        argv, out = sys.argv, io.StringIO()
        sys.argv = ['botw_prompts.py', str(mod), '--romfs', str(romfs), *args]
        code = 0
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                bp.main()
        except SystemExit as stop:
            code = stop.code
        finally:
            sys.argv = argv
        return code, out.getvalue()

    def written(self, mod):
        return sorted(p.relative_to(mod).as_posix() for p in mod.rglob('*') if p.is_file()) if mod.exists() else []

    def test_a_font_the_tool_cannot_use_is_skipped_with_a_note_and_not_written(self):
        import tempfile
        from test_botw_prompts import FontGlyphs
        good, odd = FontGlyphs('test_bc4_block_round_trip_of_flat_and_two_level_blocks').odd_fonts()
        odd['no external font'] = None
        for name, data in odd.items():
            archive = yaz0_encode(sarc({'Ancient_00.bffnt': sample(300, 1)} if data is None else {'External_00.bffnt': data}))
            with self.subTest(name), tempfile.TemporaryDirectory() as tmp:
                tmp = Path(tmp)
                romfs, _ = self.romfs_with_everything(tmp, {'Font/Font_US.sbfarc': archive, 'Font/Font_EU.sbfarc': self.font_stream})
                code, text = self.run_tool(tmp / 'botw190', romfs)
                self.assertEqual(code, 0, text)
                self.assertIn('note: Font/Font_US.sbfarc:', text)
                self.assertNotIn('note: Font/Font_EU.sbfarc', text)
                # the odd font stays the game's; the other font (negative control) and the pack are made
                self.assertEqual(self.written(tmp / 'botw190'), ['Font/Font_EU.sbfarc', 'Pack/Bootup.pack', 'manifest.tsv'])

    def test_scale_2_without_a_usable_resource_size_table_draws_at_scale_1(self):
        import tempfile
        cases = {'no table': ({bf.RSTB: None}, ('Layout/Common.blarc', 'Font/Font_US.bfarc')),
                 'no entry for the layout': ({}, ('Font/Font_US.bfarc', 'Other.bin')),
                 'no entry for a font': ({}, ('Layout/Common.blarc', 'Other.bin')),
                 'not a table': ({bf.RSTB: yaz0_encode(b'not an RSTB')}, ('Other.bin',))}
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs, _ = self.romfs_with_everything(tmp / 'ref')
            self.assertEqual(self.run_tool(tmp / 'one', romfs)[0], 0)
            self.assertEqual(self.run_tool(tmp / 'two', romfs, '--scale', '2')[0], 0)
            self.assertIn(bf.RSTB, self.written(tmp / 'two'))                    # negative control: a complete table lets it grow
            self.assertNotEqual((tmp / 'two' / bp.PACK).read_bytes(), (tmp / 'one' / bp.PACK).read_bytes())
            for case, (files, names) in cases.items():
                with self.subTest(case):
                    romfs, _ = self.romfs_with_everything(tmp / case, files, names)
                    mod = tmp / f'mod {case}'
                    code, text = self.run_tool(mod, romfs, '--scale', '2')
                    self.assertEqual(code, 0, text)
                    self.assertIn('--scale 1', text)
                    self.assertIn('note:', text)
                    self.assertEqual(self.written(mod), ['Font/Font_US.sbfarc', 'Pack/Bootup.pack', 'manifest.tsv'])        # no table, nothing grown
                    for name in ('Pack/Bootup.pack', 'Font/Font_US.sbfarc'):
                        self.assertEqual((mod / name).read_bytes(), (tmp / 'one' / name).read_bytes(), name)       # the scale 1 files

    def test_scale_2_with_a_layout_that_cannot_grow_draws_at_scale_1(self):
        import tempfile
        pack = sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/__Combined.bntx': bntx(self.pics)}))})        # a BNTX without the game's layout
        with self.assertRaises(ValueError):
            bp.make_pack(pack, self.controls, self.letters, 'xbox', True, 2)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs, _ = self.romfs_with_everything(tmp / 'game', {bp.PACK: pack})
            code, text = self.run_tool(tmp / 'two', romfs, '--scale', '2')
            self.assertEqual(code, 0, text)
            self.assertIn('note: Pack/Bootup.pack:', text)
            self.assertEqual(self.run_tool(tmp / 'one', romfs)[0], 0)
            self.assertEqual(self.written(tmp / 'two'), ['Font/Font_US.sbfarc', 'Pack/Bootup.pack', 'manifest.tsv'])
            for name in ('Pack/Bootup.pack', 'Font/Font_US.sbfarc'):
                self.assertEqual((tmp / 'two' / name).read_bytes(), (tmp / 'one' / name).read_bytes(), name)

    def test_a_missing_pack_layout_or_bntx_stops_the_run_with_a_message_before_anything_is_written(self):
        import tempfile
        no_layout = sarc({'Pack/other': sample(100, 1)})
        no_bntx = sarc({'Layout/Common.sblarc': yaz0_encode(sarc({'timg/other.bntx': bntx(self.pics)}))})
        for case, files, want in (('no pack', {bp.PACK: None}, bp.PACK), ('no layout', {bp.PACK: no_layout}, 'Layout/Common.sblarc'),
                                  ('no bntx', {bp.PACK: no_bntx}, 'timg/__Combined.bntx')):
            for args in ((), ('--scale', '2')):
                with self.subTest(case=case, args=args), tempfile.TemporaryDirectory() as tmp:
                    tmp = Path(tmp)
                    romfs, _ = self.romfs_with_everything(tmp, files)
                    code, text = self.run_tool(tmp / 'botw190', romfs, *args)
                    self.assertEqual(code, 2, text)
                    self.assertIn(want, text)
                    self.assertIn('not found', text)
                    self.assertEqual(self.written(tmp / 'botw190'), [])
        with tempfile.TemporaryDirectory() as tmp:           # negative control: the same RomFS with its pack runs
            romfs, _ = self.romfs_with_everything(Path(tmp))
            self.assertEqual(self.run_tool(Path(tmp) / 'botw190', romfs)[0], 0)

    def test_the_command_line_at_scale_2_writes_the_resource_size_table_and_refuses_an_existing_one(self):
        import tempfile
        import zlib
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs, _ = self.romfs_with_everything(tmp)
            mod = tmp / 'botw190'
            self.main_with(mod, romfs, '--scale', '2')
            layout_grown = bp.make_pack(self.pack, self.controls, self.letters, 'xbox', True, 2)[4][3]
            font_grown = bp.make_font(self.font_stream, self.controls, self.letters, 'xbox', 2)[3]
            table = bf.yaz0_decode((mod / bf.RSTB).read_bytes())
            sizes = {struct.unpack_from('<II', table, 12 + 8 * i)[0]: struct.unpack_from('<II', table, 12 + 8 * i)[1] for i in range(3)}
            self.assertEqual(sizes[zlib.crc32(b'Layout/Common.blarc')], 1000 + layout_grown)
            self.assertEqual(sizes[zlib.crc32(b'Font/Font_US.bfarc')], 2000 + font_grown)
            self.assertEqual(sizes[zlib.crc32(b'Other.bin')], 3000)
            self.assertTrue(layout_grown > 0 and font_grown > 0)
            self.assertEqual(sorted(p.relative_to(mod).as_posix() for p in mod.rglob('*') if p.is_file()),
                             ['Font/Font_US.sbfarc', 'Pack/Bootup.pack', 'System/Resource/ResourceSizeTable.product.srsizetable', 'manifest.tsv'])
            before = (mod / bf.RSTB).read_bytes()
            with self.assertRaises(SystemExit) as stop:             # the run is refused: everything is already there
                self.main_with(mod, romfs, '--scale', '2')
            self.assertEqual(stop.exception.code, 2)
            self.main_with(mod, romfs, '--scale', '2', '--replace')         # --replace writes it again, the same
            self.assertEqual((mod / bf.RSTB).read_bytes(), before)

    def test_with_mods_that_replace_the_size_table_the_icons_raise_that_table_and_name_it_as_the_original(self):
        import hashlib
        import tempfile
        import zlib
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs, entries = self.romfs_with_everything(tmp)
            mods = tmp / 'mods'                 # the mod's table: Common.blarc far above the game's, the font at the game's, Other.bin raised
            theirs = {zlib.crc32(b'Layout/Common.blarc'): 900000, zlib.crc32(b'Font/Font_US.bfarc'): 2000, zlib.crc32(b'Other.bin'): 7777}
            table = b'RSTB' + struct.pack('<II', len(entries), 0) + b''.join(struct.pack('<II', crc, theirs[crc]) for crc, _ in entries)
            (mods / bf.RSTB).parent.mkdir(parents=True)
            (mods / bf.RSTB).write_bytes(yaz0_encode(table))
            sha = hashlib.sha256((mods / bf.RSTB).read_bytes()).hexdigest()
            (mods / 'manifest.tsv').write_text(f'# header\n{bf.RSTB}\t{(mods / bf.RSTB).stat().st_size}\t{sha}\t{"0" * 64}\n')
            mod = tmp / 'botw190'
            self.main_with(mod, romfs, '--scale', '2', '--mods', str(mods))
            font_grown = bp.make_font(self.font_stream, self.controls, self.letters, 'xbox', 2)[3]
            out = bf.yaz0_decode((mod / bf.RSTB).read_bytes())
            sizes = dict(struct.unpack_from('<II', out, 12 + 8 * i) for i in range(3))
            self.assertEqual(sizes[zlib.crc32(b'Layout/Common.blarc')], 900000)         # the mod's is larger than the game's grown one
            self.assertEqual(sizes[zlib.crc32(b'Font/Font_US.bfarc')], 2000 + font_grown)
            self.assertEqual(sizes[zlib.crc32(b'Other.bin')], 7777)                     # the mod's other entries are kept
            line = [l for l in (mod / 'manifest.tsv').read_text().splitlines() if l.startswith(bf.RSTB + '\t')][0]
            self.assertEqual(line.split('\t')[3], sha)                                  # romfs.c: it takes the mod table's place
            (mods / 'manifest.tsv').write_text('# header\n')                             # mods without a table: the game's is raised
            self.main_with(mod, romfs, '--scale', '2', '--mods', str(mods), '--replace')
            out = bf.yaz0_decode((mod / bf.RSTB).read_bytes())
            self.assertEqual(dict(struct.unpack_from('<II', out, 12 + 8 * i) for i in range(3))[zlib.crc32(b'Other.bin')], 3000)
            (mods / 'manifest.tsv').write_text(f'# header\n{bf.RSTB}\t{(mods / bf.RSTB).stat().st_size}\t{sha}\t{"0" * 64}\n')
            self.main_with(mods, romfs, '--scale', '2', '--mods', str(mods), '--replace')   # written into the mods folder itself
            out = bf.yaz0_decode((mods / bf.RSTB).read_bytes())
            self.assertEqual(dict(struct.unpack_from('<II', out, 12 + 8 * i) for i in range(3))[zlib.crc32(b'Other.bin')], 7777)
            line = [l for l in (mods / 'manifest.tsv').read_text().splitlines() if l.startswith(bf.RSTB + '\t')][0]
            with (romfs / 'romfs.bin').open('rb') as image:
                offset, size = bp.read_index(romfs / 'runtime-index.bin')[bf.RSTB]; image.seek(offset); game = hashlib.sha256(image.read(size)).hexdigest()
            self.assertEqual(line.split('\t')[3], game)                                 # the original is the game's table
            (mods / 'manifest.tsv').write_text(f'{bf.RSTB}\t1\t{"1" * 64}\t-\n')        # a table that does not match its line:
            fresh = tmp / 'fresh'                                                          # the icons are drawn at 1x, with no table
            self.main_with(fresh, romfs, '--scale', '2', '--mods', str(mods), '--version', 'botw190')
            self.assertTrue((fresh / bp.PACK).is_file())
            self.assertFalse((fresh / bf.RSTB).exists())

    def test_an_existing_resource_size_table_alone_stops_a_scale_2_run_but_not_a_scale_1_run(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs, _ = self.romfs_with_everything(tmp)
            mod = tmp / 'botw190'
            (mod / bf.RSTB).parent.mkdir(parents=True)
            (mod / bf.RSTB).write_bytes(b'another mod')
            with self.assertRaises(SystemExit) as stop:
                self.main_with(mod, romfs, '--scale', '2')
            self.assertEqual(stop.exception.code, 2)
            self.assertEqual(sorted(p.relative_to(mod).as_posix() for p in mod.rglob('*') if p.is_file()), [bf.RSTB])
            self.assertEqual((mod / bf.RSTB).read_bytes(), b'another mod')
            self.main_with(mod, romfs)                                      # scale 1 never touches the table
            self.assertEqual((mod / bf.RSTB).read_bytes(), b'another mod')
            self.assertTrue((mod / bp.PACK).is_file())


def runtime_index(files):
    """A runtime-index.bin for `files` (RomFS path -> (offset, size)), as make_mod_manifest.read_index reads it."""
    names = b''.join(n.encode() + b'\0' for n in files)
    entries, at = b'', 0
    for name, (offset, size) in files.items():
        entries += struct.pack('<QQII', offset, size, at, len(name) + 1); at += len(name) + 1
    return struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(files) * 24) + entries + names


@unittest.skipIf(bf is None, 'needs Python 3.14')
class GrownLetters(unittest.TestCase):
    """grown_contour() on a letter A (an outer shape with an acute triangular counter) cut out of a disc, as the button drawings
    do (the letter is drawn the other way round), at 48 x 48 pixels. The counter of this A has an inscribed circle of 3 pixels."""
    OUTER = [(562, 239), (651, 0), (759, 0), (452, 807), (317, 807), (10, 0), (118, 0), (207, 239)]
    COUNTER = [(530, 325), (238, 325), (384, 718)]          # turned the other way: a hole

    def counters(self, stroke):
        """The white marks inside the letter once its outline has grown by `stroke` pixels."""
        place = lambda contour: [(24 + (x - 385) * 0.03, 24 + (403 - y) * 0.03) for x, y in contour][::-1]
        disc = [(24 + 22 * math.cos(t * math.pi / 32), 24 + 22 * math.sin(t * math.pi / 32)) for t in range(64)]
        grown = [bp.grown_contour(place(c), stroke) for c in (self.OUTER, self.COUNTER)]
        rows = bf.coverage([disc] + [c[::-1] for c in grown if c], 48)
        return [len(m) for m in bf.enclosed_marks([bytes(255 - v for v in row) for row in rows], window=(6, 6, 42, 42), largest=80)]

    def test_a_counter_the_stroke_closes_is_solid(self):
        for stroke in (3.2, 4.0):
            self.assertEqual(self.counters(stroke), [], stroke)

    def test_a_counter_the_stroke_does_not_close_stays_open_and_shrinks_without_specks(self):
        sizes = []
        for stroke in (1.0, 2.0, 2.5):
            marks = self.counters(stroke)
            self.assertEqual(len(marks), 1, stroke)             # the counter, and no speck beside it
            sizes.append(marks[0])
        self.assertTrue(sizes[0] > sizes[1] > sizes[2] > 0, sizes)

    def test_an_inward_corner_is_not_cut_off(self):
        hole = bp.grown_contour(self.COUNTER, 5)
        self.assertEqual(len(hole), 3)                          # the plain mitre point at each corner of the hole
        self.assertEqual(bp.grown_contour(self.COUNTER, 200), [])     # shrunk to nothing: the counter fills


class CommandLine(unittest.TestCase):
    def test_a_font_already_in_the_mod_folder_stops_the_run_before_anything_is_written(self):
        import tempfile
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            romfs = tmp / 'game' / 'botw190' / 'romfs'
            romfs.mkdir(parents=True)
            (romfs / 'romfs.bin').write_bytes(bytes(64))
            (romfs / 'runtime-index.bin').write_bytes(runtime_index({bp.PACK: (0, 32), 'Font/Font_US.sbfarc': (32, 32)}))
            (tmp / 'game' / 'firmware' / bp.FONT).parent.mkdir(parents=True)
            (tmp / 'game' / 'firmware' / bp.FONT).write_bytes(b'font')
            mod = tmp / 'botw190'
            (mod / 'Font').mkdir(parents=True)
            (mod / 'Font' / 'Font_US.sbfarc').write_bytes(b'another mod')
            argv = sys.argv
            sys.argv = ['botw_prompts.py', str(mod), '--romfs', str(romfs)]
            try:
                with self.assertRaises(SystemExit) as stop:
                    bp.main()
            finally:
                sys.argv = argv
            self.assertEqual(stop.exception.code, 2)
            self.assertEqual(sorted(p.relative_to(mod).as_posix() for p in mod.rglob('*')), ['Font', 'Font/Font_US.sbfarc'])
            self.assertEqual((mod / 'Font' / 'Font_US.sbfarc').read_bytes(), b'another mod')

POPUP_GROUP = {'a': (26.0, 0.0), 'b': (0.0, -26.0), 'x': (0.0, 26.0), 'y': (-26.0, 0.0)}       # the pressed disc of each Switch popup
POPUP_LETTER = {'a': (26.2, 0.8), 'b': (1.0, -26.0), 'x': (0.8, 26.0), 'y': (-25.5, -0.2)}     # the faint letters
POPUP_BASE = {'a': ('AL', 0.0, 1.0), 'b': ('BL', 0.0, 1.0), 'x': ('BL', 180.0, -1.0), 'y': ('AL', 180.0, 1.0)}   # texture, turn, x scale
POPUP_CAPTURE = {'a': (86.0, 4.0), 'b': (60.0, -26.0), 'x': (60.0, 26.0), 'y': (-86.0, 0.0)}


def popup_bflyt(slot):
    """A made-up action popup (PaDoCommand<slot>_00) laid out as the game's: faint letters on the other spots, the pressed disc's
    group with its label box (opening to the left on the left spot), the backdrop turned to cut at the disc, and a capture pane."""
    def pane(magic, name, x, y, rz=0.0, sx=1.0, origin=1, extra=b''):
        body = bytes([0, 255, origin, 0]) + name.encode().ljust(24, b'\0') + bytes(8) + struct.pack('<10f', x, y, 0, 0, 0, rz, sx, 1, 30, 40)
        return magic + struct.pack('<I', 8 + len(body) + len(extra)) + body + extra

    def text(name, x, y, letters, position=0, origin=1):       # a text pane: its text at 104, room for `letters` and the end
        room = 2 * len(letters) + 2
        return pane(b'txt1', name, x, y, origin=origin,
                    extra=struct.pack('<4H2BH4xI', room, room, 0, 0, position, 0, 0, 104) + letters.encode('utf-16-le') + bytes(2))
    S, (sub, rz, sx), left = slot.upper(), POPUP_BASE[slot], slot == 'y'
    textures = ['FBLayout_00^t', f'Nt_KeyBtnSubBase{sub}_00^s', f'Nt_KeyTex{S}_00^d']
    strings, offsets = b'', []
    for t in textures:
        offsets.append(4 * len(textures) + len(strings))
        strings += t.encode() + b'\0'
    txl = struct.pack('<H2x', len(textures)) + struct.pack(f'<{len(textures)}I', *offsets) + strings
    txl += bytes(-len(txl) % 4)
    opened, closed = b'pas1' + struct.pack('<I', 8), b'pae1' + struct.pack('<I', 8)
    sections = [b'txl1' + struct.pack('<I', 8 + len(txl)) + txl, pane(b'pan1', 'RootPane', 0, 0), opened,
                pane(b'pan1', f'N_Capture{S}_00', *POPUP_CAPTURE[slot]), opened, pane(b'pic1', f'Nt_KeyTex{S}_01', 60 if left else -60, 0), closed,
                pane(b'pan1', 'N_Loop_00', 0, 0), opened, pane(b'pic1', 'P_Base_00' if slot == 'a' else f'P_Base{S}_00', 0, 0, rz, sx)]
    sections += [text(f'T_{q.upper()}_00', *POPUP_LETTER[q], q.upper()) for q in 'abxy' if q != slot]
    sections += [pane(b'pan1', 'N_BtnX_00' if left else f'N_Btn{S}_00', *POPUP_GROUP[slot]), opened,      # (the game's Y popup names it so)
                 pane(b'wnd1', f'W_Base{S}_00', 33 if left else -33, 0, sx=-1 if left else 1),
                 text(f'T_Guide{S}_01', -23 if left else 21, 0, 'Open', 2 if left else 1, 2 if left else 1),
                 pane(b'pic1', f'Nt_KeyTex{S}_00', 0, 0), closed, closed]
    if slot == 'a':
        sections += [pane(b'pan1', 'N_Attention_00', 0, 0), opened, pane(b'pic1', 'P_CaptureUse_00', 86, 4), closed]
    sections.append(closed)
    body = b''.join(sections)
    return b'FLYT' + b'\xff\xfe' + struct.pack('<HIIHH', 0x14, 0, 0x14 + len(body), len(sections), 0) + body


@unittest.skipIf(bf is None, 'needs Python 3.14')
class ActionPopups(unittest.TestCase):
    """The action popup by a chest ("A: Open"): the style's letters at the pad's own spots and the pressed disc where it is."""
    OWNER = 'north = a\nsouth = x\nwest = y\neast = b\n'
    XBOX = {'x': 'Y', 'y': 'X', 'a': 'B', 'b': 'A'}       # Switch spot -> the Xbox letter there (Y on top, X left, B right, A bottom)
    LOOK = ('box', 'guide', 'base', 'capture')
    SIZE = 48       # (for Scale.setUp)

    def setUp(self):
        self.layout = sarc({'blyt/other.bflyt': sample(3000, 5), **{bp.POPUP.format(s.upper()): popup_bflyt(s) for s in 'abxy'}})
        self.inner = bf.sarc_members(self.layout)

    def read(self, layout, slot):
        """The faint letters ({spot: letter}), the spot of the disc's group, the label box's x scale, the guide text's anchor and
        text position, the backdrop (texture, turn, x scale) and the capture pane's place of the popup of `slot` in `layout`."""
        lo, hi = bf.sarc_members(layout)[bp.POPUP.format(slot.upper())]
        blob = bytes(layout[lo:hi])
        panes, textures, group = bp._popup(blob, slot)

        def near(x, y):
            return min(POPUP_GROUP, key=lambda s: (POPUP_GROUP[s][0] - x) ** 2 + (POPUP_GROUP[s][1] - y) ** 2)

        def field(name, at):
            return struct.unpack_from('<f', blob, panes[name][0] + at)[0]
        letters = {}
        for name, (offset, _) in panes.items():
            if bp.LETTER.fullmatch(name):
                letters[near(*struct.unpack_from('<2f', blob, offset + 44))] = blob[offset + 104:offset + 106].decode('utf-16-le')
        S, base = slot.upper(), next(n for n in panes if n.startswith('P_Base'))
        guide = panes[f'T_Guide{S}_01'][0]
        return {'letters': letters, 'disc': near(field(group, 44), field(group, 48)), 'box': field(f'W_Base{S}_00', 68),
                'guide': (field(f'T_Guide{S}_01', 44), blob[guide + 10], blob[guide + 92]),
                'base': (next(t for t in textures if 'SubBase' in t)[16:18], field(base, 64), field(base, 68)),
                'capture': (field(f'N_Capture{S}_00', 44), field(f'N_Capture{S}_00', 48))}

    def edited(self, controls, style='xbox', switch=False):
        edits, notes = bp.popup_edits(self.layout, self.inner, controls, style, bp.SWITCH_SPOT if switch else bp.SPOT)
        return bf.apply_edits(self.layout, edits), notes

    def test_the_default_layout_writes_the_xbox_letters_and_moves_nothing(self):
        layout, notes = self.edited(pl.prompt_controls('', 'position'))
        self.assertEqual(notes, [])
        self.assertEqual(len(layout), len(self.layout))
        for slot in 'abxy':
            got, old = self.read(layout, slot), self.read(self.layout, slot)
            self.assertEqual(got['letters'], {q: self.XBOX[q] for q in 'abxy' if q != slot}, slot)
            self.assertEqual(got['disc'], slot)
            for key in self.LOOK:
                self.assertEqual(got[key], old[key], (slot, key))

    def test_the_owner_s_a_shows_y_on_top_laid_out_as_the_game_s_top_popup(self):
        layout, _ = self.edited(pl.prompt_controls(self.OWNER, 'position'))      # Switch A <- Xbox Y
        got, top = self.read(layout, 'a'), self.read(self.layout, 'x')
        self.assertEqual(got['disc'], 'x')
        self.assertEqual(got['letters'], {'y': 'X', 'a': 'B', 'b': 'A'})        # the X letter pane moved to the free right spot
        for key in self.LOOK:
            self.assertEqual(got[key], top[key], key)

    def test_the_left_spot_opens_the_label_box_to_the_left_and_back(self):
        layout, _ = self.edited(pl.prompt_controls('', 'label'))       # Switch X <- Xbox X (left), Y <- Y (top)
        for slot, spot in (('x', 'y'), ('y', 'x')):
            got, game = self.read(layout, slot), self.read(self.layout, spot)
            self.assertEqual(got['disc'], spot)
            for key in self.LOOK:
                self.assertEqual(got[key], game[key], (slot, key))
            self.assertEqual(got['letters'], {q: self.XBOX[q] for q in 'abxy' if q != spot})
        self.assertNotEqual(self.read(self.layout, 'y')['box'], self.read(self.layout, 'x')['box'])     # the two really differ

    def test_the_flash_on_the_label_box_moves_with_it(self):
        layout, _ = self.edited(pl.prompt_controls(self.OWNER, 'position'))
        lo, hi = bf.sarc_members(layout)[bp.POPUP.format('A')]
        panes, _, _ = bp._popup(bytes(layout[lo:hi]), 'a')
        shift = tuple(b - a for a, b in zip(POPUP_CAPTURE['a'], POPUP_CAPTURE['x']))
        self.assertEqual(struct.unpack_from('<2f', layout, lo + panes['N_Attention_00'][0] + 44), shift)

    def test_playstation_leaves_the_letters_blank_and_moves_the_disc(self):
        layout, _ = self.edited(pl.prompt_controls(self.OWNER, 'position'), 'playstation')
        got = self.read(layout, 'a')
        self.assertEqual(got['disc'], 'x')
        self.assertEqual(set(got['letters'].values()), {' '})

    def test_the_switch_style_keeps_the_game_s_popups_and_moves_a_remapped_disc(self):
        layout, _ = self.edited(pl.prompt_controls('', 'label'), 'switch', True)
        self.assertEqual(layout, bytes(self.layout))
        layout, _ = self.edited(pl.prompt_controls(self.OWNER, 'label'), 'switch', True)     # Switch A <- X (top)
        got = self.read(layout, 'a')
        self.assertEqual((got['disc'], got['letters']), ('x', {'y': 'Y', 'a': 'A', 'b': 'B'}))

    def test_a_button_no_face_button_drives_keeps_its_popup(self):
        layout, notes = self.edited({**pl.prompt_controls('', 'position'), 'a': 'l1'})
        lo, hi = self.inner[bp.POPUP.format('A')]
        self.assertEqual(layout[lo:hi], self.layout[lo:hi])
        self.assertTrue(any(n.startswith('A:') and 'popup keeps' in n for n in notes))

    def test_a_missing_popup_edits_nothing(self):      # negative control: another game version
        layout = sarc({bp.POPUP.format(s.upper()): popup_bflyt(s) for s in 'abx'})
        edits, notes = bp.popup_edits(layout, bf.sarc_members(layout), pl.prompt_controls(self.OWNER, 'position'), 'xbox')
        self.assertEqual(edits, [])
        self.assertTrue(any('action popup' in n for n in notes), notes)

    @unittest.skipIf(mbi is None, 'needs fontTools')
    def test_make_pack_edits_the_popups_the_same_at_every_scale(self):
        Scale.setUp(self)
        layout = sarc({**{n: self.layout[slice(*r)] for n, r in bf.sarc_members(self.layout).items()},
                       **{bp.POPUP.format(s.upper()): popup_bflyt(s) for s in 'abxy'}})
        pack = sarc({'Layout/Common.sblarc': yaz0_encode(layout)})
        controls = pl.prompt_controls(self.OWNER, 'position')
        want = bf.apply_edits(layout, bp.popup_edits(layout, bf.sarc_members(layout), controls, 'xbox')[0])
        a = bf.sarc_members(want)
        for scale in (1, 2, 4):
            new_pack = bp.make_pack(pack, controls, self.letters, 'xbox', True, scale)[0]
            new = bf.yaz0_decode(new_pack[slice(*bf.sarc_members(new_pack)['Layout/Common.sblarc'])])
            b = bf.sarc_members(new)
            for s in 'ABXY':
                self.assertEqual(want[slice(*a[bp.POPUP.format(s)])], new[slice(*b[bp.POPUP.format(s)])], (scale, s))


if __name__ == '__main__':
    unittest.main()
