"""PlayStation button icons for Breath of the Wild 1.9.0 (make_botw_button_icons.py --style playstation). Standard library only.

The game's font has no PlayStation symbols, so they are drawn here, in the game's own style: each icon is the game's white
shape (the disc of A B X Y, + and -, the key of L R ZL ZR) with the symbol cut out of it. The shape is the game's own icon
with its letter filled in (silhouette), and the symbol is cut from it with strokes as thick as the game's letters:
  font (Font/Font_*.sbfarc -> External_00.bffnt, a BC4 glyph sheet of 50x50 cells): the cell of the A glyph becomes Cross,
      B Circle, X Square, Y Triangle (the code points the Xbox letters use for the bottom, right, left and top buttons, so
      the remap of make_botw_button_icons.py points each input at the right symbol), L R ZL ZR become L1 R1 L2 R2, + Options
      and - Create. The sheet keeps its size: only the BC4 blocks of these cells are encoded again.
  action guide (Pack/Bootup.pack -> Layout/Common.sblarc -> timg/__Combined.bntx, 48x48 R8 block-linear textures): the key
      textures A B X Y (the diamond draws them by place: A bottom, B right, X left, Y top) become Cross, Circle, Square and
      Triangle; L and ZL (the guide of the L and ZL inputs) take the label of the shoulder button that drives them.
Nothing of the game is copied into the project: every picture is made from the player's own files at run time."""
import math
import re
import struct

GLYPH_SYMBOL = {0xE040: 'cross', 0xE041: 'circle', 0xE042: 'square', 0xE043: 'triangle', 0xE044: 'L1', 0xE045: 'R1',
                0xE046: 'L2', 0xE047: 'R2', 0xE04B: 'options', 0xE04C: 'create'}
TEXTURE_SYMBOL = {'Nt_KeyTexA_00^d': 'cross', 'Nt_KeyTexB_00^d': 'circle', 'Nt_KeyTexX_00^d': 'square',
                  'Nt_KeyTexY_00^d': 'triangle'}
SHOULDER_TEXTURE = {'Nt_KeyTexL_00^d': 0xE044, 'Nt_KeyTexZL_00^d': 0xE046}   # texture -> the input's code point
SHOULDER_LABEL = {0xE044: 'L1', 0xE045: 'R1', 0xE046: 'L2', 0xE047: 'R2'}
BNTX = 'timg/__Combined.bntx'


# ---- BNTX textures (block-linear) and BC4 ----------------------------------------------------------------------------------
R8_UNORM, BC4_UNORM = 0x201, 0x1d01


class Texture:
    """One BRTI of a BNTX: where its pixels are and how they are laid out."""
    def __init__(self, bntx, brti):
        (self.tile_mode, _, self.mips, _, self.format, _, self.width, self.height, self.depth, self.layers,
         layout) = struct.unpack_from('<HHHIIIIIIII', bntx, brti + 0x12)
        self.block_height = 1 << (layout & 7)
        self.image_size = struct.unpack_from('<I', bntx, brti + 0x50)[0]
        name = struct.unpack_from('<Q', bntx, brti + 0x60)[0]
        self.name = bntx[name + 2:name + 2 + struct.unpack_from('<H', bntx, name)[0]].decode()
        self.offset = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, brti + 0x70)[0])[0]

    def check(self, fmt):
        if (self.format, self.tile_mode, self.mips, self.layers, self.depth) != (fmt, 0, 1, 1, 1):
            raise ValueError(f'{self.name}: format 0x{self.format:x}, tile mode {self.tile_mode}, {self.mips} mips: '
                             f'expected a plain 0x{fmt:x} block-linear texture')


def textures(bntx):
    """name -> Texture for every texture of a BNTX."""
    if bntx[:4] != b'BNTX':
        raise ValueError('not a BNTX')
    return {t.name: t for t in (Texture(bntx, m.start()) for m in re.finditer(b'BRTI', bntx))}


def gob_address(x, y, width, element, block_height):
    """Byte offset of element (x, y) of a block-linear texture whose elements (a pixel, or a 4x4 block) are `element` bytes."""
    xb = x * element
    gobs_wide = (width * element + 63) // 64
    gob = (y // (8 * block_height)) * 512 * block_height * gobs_wide + (xb // 64) * 512 * block_height + \
        (y % (8 * block_height) // 8) * 512
    return gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)


def r8_read(bntx, t):
    t.check(R8_UNORM)
    return [bytearray(bntx[t.offset + gob_address(x, y, t.width, 1, t.block_height)] for x in range(t.width))
            for y in range(t.height)]


def r8_write(out, t, rows):
    """Write rows (top first) into the texture's pixels in out (the BNTX, a bytearray)."""
    for y, row in enumerate(rows):
        for x, v in enumerate(row):
            out[t.offset + gob_address(x, y, t.width, 1, t.block_height)] = v


def bc4_decode(block):
    r0, r1 = block[0], block[1]
    palette = [r0, r1] + ([((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)] if r0 > r1 else
                          [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255])
    bits = int.from_bytes(block[2:8], 'little')
    return [palette[(bits >> (3 * i)) & 7] for i in range(16)]


def bc4_encode(values):
    """An 8-byte BC4 block for 16 values: the eight-step mode between the extremes, or the six-step one (with 0 and 255
    exact) between the values in between, whichever is closer."""
    hi, lo = max(values), min(values)
    if hi == lo:
        return bytes([hi, lo]) + bytes(6)
    modes = [(bytes([hi, lo]), [hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)])]
    inner = [v for v in values if 0 < v < 255]
    if inner and max(inner) > min(inner):
        a, b = min(inner), max(inner)              # r0 < r1: a, b, four steps between, then 0 and 255
        modes.append((bytes([a, b]), [a, b] + [((5 - i) * a + i * b) // 5 for i in range(1, 5)] + [0, 255]))
    best = None
    for head, palette in modes:
        picks = [min(range(8), key=lambda k: abs(palette[k] - v)) for v in values]
        error = sum(abs(palette[k] - v) for k, v in zip(picks, values))
        if best is None or error < best[0]:
            best = (error, head, picks)
    _, head, picks = best
    return head + sum(k << (3 * i) for i, k in enumerate(picks)).to_bytes(6, 'little')


# ---- the glyph sheet of External_00.bffnt ------------------------------------------------------------------------------------
class Sheet:
    """The one BC4 glyph sheet of a BFFNT, its grid and its CMAP (method 1). A cell is upright in cell(), the sheet stores it
    upside down, one pixel apart, from the top left."""
    def __init__(self, ffnt):
        if ffnt[:4] != b'FFNT':
            raise ValueError('not a BFFNT')
        position, sections = struct.unpack_from('<H', ffnt, 6)[0], {}
        for _ in range(struct.unpack_from('<H', ffnt, 16)[0]):
            sections[ffnt[position:position + 4]] = position
            position += struct.unpack_from('<I', ffnt, position + 4)[0]
        tglp = sections[b'TGLP']
        (self.w, self.h, sheets, _, size, _, _, self.columns, _, _, _, self.at) = struct.unpack_from('<BBBBIHHHHHHI', ffnt,
                                                                                                    tglp + 8)
        if sheets != 1:
            raise ValueError(f'{sheets} glyph sheets, expected one')
        [self.texture] = textures(ffnt[self.at:self.at + size]).values()
        self.texture.check(BC4_UNORM)
        cmap = sections[b'CMAP']
        lo, hi, method = struct.unpack_from('<IIH', ffnt, cmap + 8)
        if method != 1:
            raise ValueError(f'CMAP method {method}, expected 1')
        self.index = {lo + i: v for i, v in enumerate(struct.unpack_from(f'<{hi - lo + 1}H', ffnt, cmap + 24)) if v != 0xFFFF}
        self.ffnt = ffnt

    def image(self):
        """(offset in the BFFNT, length) of the sheet's pixels."""
        return self.at + self.texture.offset, self.texture.image_size

    def box(self, code):
        i, t = self.index[code], self.texture
        x0 = 1 + i % self.columns * (self.w + 1)
        y0 = t.height - (i // self.columns + 1) * (self.h + 1)
        return x0, y0, x0 + self.w, y0 + self.h

    def _block(self, image, bx, by):
        at = gob_address(bx, by, self.texture.width // 4, 8, self.texture.block_height)
        return at, image[at:at + 8]

    def cell(self, image, code):
        """Upright rows of the glyph of code, read from the sheet's pixels."""
        x0, y0, x1, y1 = self.box(code)
        rows = {}
        for by in range(y0 // 4, (y1 + 3) // 4):
            for bx in range(x0 // 4, (x1 + 3) // 4):
                for i, v in enumerate(bc4_decode(self._block(image, bx, by)[1])):
                    rows.setdefault(by * 4 + i // 4, {})[bx * 4 + i % 4] = v
        return [bytearray(rows[y][x] for x in range(x0, x1)) for y in reversed(range(y0, y1))]

    def put(self, image, code, cell):
        """Write an upright cell into image (the sheet's pixels, a bytearray): the blocks it touches are encoded again, with
        the pixels of their other cells as they were."""
        x0, y0, x1, y1 = self.box(code)
        for by in range(y0 // 4, (y1 + 3) // 4):
            for bx in range(x0 // 4, (x1 + 3) // 4):
                at, old = self._block(image, bx, by)
                values = bc4_decode(old)
                for i in range(16):
                    x, y = bx * 4 + i % 4, by * 4 + i // 4
                    if x0 <= x < x1 and y0 <= y < y1:
                        values[i] = cell[y1 - 1 - y][x - x0]
                image[at:at + 8] = bc4_encode(values)


# ---- drawing --------------------------------------------------------------------------------------------------------------
def silhouette(rows, dark=128):
    """The icon's white shape with its letter filled in: every pixel the dark outside (reached from the border through
    pixels below `dark`) does not reach becomes 255; the outside and the shape's soft edge keep their values."""
    h, w = len(rows), len(rows[0])
    outside, todo = set(), [(x, y) for x in range(w) for y in (0, h - 1)] + [(x, y) for y in range(h) for x in (0, w - 1)]
    while todo:
        x, y = todo.pop()
        if (x, y) in outside or not (0 <= x < w and 0 <= y < h) or rows[y][x] >= dark:
            continue
        outside.add((x, y))
        todo += [(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)]
    edge = {(x + dx, y + dy) for x, y in outside for dx in (-1, 0, 1) for dy in (-1, 0, 1)}
    return [bytearray(rows[y][x] if (x, y) in outside or (x, y) in edge else 255 for x in range(w)) for y in range(h)]


def bright_box(rows, level=128):
    pts = [(x, y) for y, r in enumerate(rows) for x, v in enumerate(r) if v >= level]
    if not pts:
        raise ValueError('no shape in the icon')
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def arc(cx, cy, r, a0, a1, steps=24):
    """Points of an arc (degrees, counter-clockwise on screen, y down)."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
             cy - r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]


# Strokes of the letters and digits, in a box 1 high (y down, 0 at the top), x from 0 to the advance.
GLYPHS = {
    'L': (0.62, [[(0.08, 0.0), (0.08, 1.0), (0.58, 1.0)]]),
    'R': (0.70, [[(0.08, 1.0), (0.08, 0.0), (0.36, 0.0)] + arc(0.36, 0.26, 0.26, 90, -90, 16) + [(0.08, 0.52)],
                 [(0.34, 0.52), (0.64, 1.0)]]),
    '1': (0.55, [[(0.10, 0.20), (0.34, 0.0), (0.34, 1.0)]]),
    '2': (0.66, [arc(0.32, 0.28, 0.26, 165, -30, 16) + [(0.06, 1.0), (0.62, 1.0)]]),
}


def label_strokes(text, cx, cy, height):
    """Strokes of text (GLYPHS) centred on (cx, cy), capitals `height` pixels high."""
    width = sum(GLYPHS[c][0] for c in text) + 0.12 * (len(text) - 1)
    x, out = cx - width * height / 2, []
    for c in text:
        advance, lines = GLYPHS[c]
        out += [[(x + px * height, cy - height / 2 + py * height) for px, py in line] for line in lines]
        x += (advance + 0.12) * height
    return out


def symbol_strokes(symbol, cx, cy, d):
    """Strokes (polylines) of a symbol centred on (cx, cy) on a disc d pixels across, and the stroke width."""
    w = 0.12 * d
    if symbol == 'cross':
        r = 0.18 * d
        return [[(cx - r, cy - r), (cx + r, cy + r)], [(cx - r, cy + r), (cx + r, cy - r)]], w
    if symbol == 'circle':
        return [arc(cx, cy, 0.22 * d, 0, 360, 48)], w
    if symbol == 'square':
        r = 0.19 * d
        return [[(cx - r, cy - r), (cx + r, cy - r), (cx + r, cy + r), (cx - r, cy + r), (cx - r, cy - r)]], w
    if symbol == 'triangle':
        r = 0.28 * d; cy += 0.04 * d
        pts = [(cx + r * math.cos(math.radians(a)), cy - r * math.sin(math.radians(a))) for a in (90, 210, 330, 90)]
        return [pts], w
    if symbol == 'options':                         # three bars
        r, gap = 0.22 * d, 0.17 * d
        return [[(cx - r, cy + k * gap), (cx + r, cy + k * gap)] for k in (-1, 0, 1)], 0.10 * d
    if symbol == 'create':                          # three rays from the bottom (the Create button's mark)
        base, n = (cx, cy + 0.24 * d), 0.42 * d
        return [[(base[0] + 0.14 * d * math.cos(math.radians(a)), base[1] - 0.14 * d * math.sin(math.radians(a))),
                 (base[0] + n * math.cos(math.radians(a)), base[1] - n * math.sin(math.radians(a)))]
                for a in (55, 90, 125)], 0.10 * d
    raise KeyError(symbol)


def stroke_coverage(strokes, width, w, h, samples=4):
    """Coverage 0..1 of round-capped strokes (polylines) of the given width over a w x h picture."""
    segs = [(a, b) for line in strokes for a, b in zip(line, line[1:])]
    half2 = (width / 2) ** 2
    xs = [p[0] for s in segs for p in s]; ys = [p[1] for s in segs for p in s]
    x0, x1 = max(0, int(min(xs) - width)), min(w, int(max(xs) + width) + 2)
    y0, y1 = max(0, int(min(ys) - width)), min(h, int(max(ys) + width) + 2)
    out = [[0.0] * w for _ in range(h)]
    offs = [(i + 0.5) / samples for i in range(samples)]
    for py in range(y0, y1):
        for px in range(x0, x1):
            hit = 0
            for oy in offs:
                y = py + oy
                for ox in offs:
                    x = px + ox
                    for (ax, ay), (bx, by) in segs:
                        dx, dy = bx - ax, by - ay
                        t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
                        if (x - ax - t * dx) ** 2 + (y - ay - t * dy) ** 2 <= half2:
                            hit += 1
                            break
            out[py][px] = hit / samples ** 2
    return out


def redraw(rows, symbol):
    """The icon `rows` (upright, white shape with a dark letter) as its shape with `symbol` cut out: a PlayStation shape on a
    disc, or a shoulder label (L1 R1 L2 R2) on a key, centred on the shape and sized from it."""
    shape = silhouette(rows)
    x0, y0, x1, y1 = bright_box(shape)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    if symbol in SHOULDER_LABEL.values():
        hgt = 0.46 * (y1 - y0)
        strokes, width = label_strokes(symbol, cx, cy, hgt), 0.17 * hgt
    else:
        strokes, width = symbol_strokes(symbol, cx, cy, min(x1 - x0, y1 - y0))
    cut = stroke_coverage(strokes, width, len(rows[0]), len(rows))
    return [bytearray(round(v * (1 - c)) for v, c in zip(r, cr)) for r, cr in zip(shape, cut)]


# ---- the files ------------------------------------------------------------------------------------------------------------
def font(ffnt, cache=None):
    """External_00.bffnt with the cells of GLYPH_SYMBOL redrawn (same size). cache: {(code, old cell): new cell}, shared by
    the seven region archives, which carry the same glyphs."""
    sheet, out = Sheet(ffnt), bytearray(ffnt)
    at, size = sheet.image()
    image = bytearray(ffnt[at:at + size])
    cache = {} if cache is None else cache
    for code, symbol in GLYPH_SYMBOL.items():
        if code not in sheet.index:
            continue
        old = sheet.cell(image, code)
        key = (code, b''.join(old))
        if key not in cache:
            cache[key] = redraw(old, symbol)
        sheet.put(image, code, cache[key])
    out[at:at + size] = image
    return bytes(out)


def sheet_span(ffnt):
    """(start, end) of the glyph sheet's pixels in a BFFNT: what font() may change."""
    at, size = Sheet(ffnt).image()
    return at, at + size


def layout_textures(bntx, glyphs):
    """__Combined.bntx with the key textures drawn for a PlayStation controller (same size): A B X Y by place, L and ZL with
    the label of the shoulder button that drives them (glyphs: the remap, input code point -> the code point shown; an input
    not in it shows its own), left as they are when no shoulder button drives them."""
    found, out = textures(bntx), bytearray(bntx)
    wanted = dict(TEXTURE_SYMBOL)
    for name, code in SHOULDER_TEXTURE.items():
        shown = glyphs.get(code, code)
        if shown in SHOULDER_LABEL:
            wanted[name] = SHOULDER_LABEL[shown]
    for name, symbol in wanted.items():
        if name in found:
            r8_write(out, found[name], redraw(r8_read(bntx, found[name]), symbol))
    return bytes(out)
