#!/usr/bin/env python3
"""Make a RomFS mod that draws keyboard keys, Xbox or PlayStation controller buttons in place of the Switch button icons.

The A/B/X/Y, L/R, ZL/ZR, +/- and d-pad icons the game shows in its prompts are glyphs of one font,
scft/External.bfotf (private-use code points U+E040 to U+E068). Every font archive has its own copy: Korean and both
Chinese read Font/Font_KRko, Font_CNzh and Font_TWzh.Nin_NX_NVN.bfarc.zs, every other language
Font/Font.Nin_NX_NVN.bfarc.zs (1.4.3: the four copies are byte-identical). This script reads those archives from the
player's own RomFS, redraws those glyphs, packs each archive back the way the game stores it and writes them into a mod
folder (runtime/romfs.c serves them in place of the originals), then writes the folder's manifest.tsv with
make_mod_manifest.py. The letters on the keys come from the text fonts of the default archive (the regional ones lack
them). Nothing of the game ships with the project: every font comes from the player's RomFS. The default archive is the
same in 1.0.0 and 1.2.1 (same SHA-256), so one mod folder per version is enough.

Styles:
  keyboard  the keys the Mac keyboard plays (app/launcher.m): A B X Y = J K I U, L R = Q E, ZL ZR = Option Shift,
            + - = Enter Tab, left stick = W A S D, right stick = arrow keys, stick presses = Z C, d-pad = T F G H.
  azerty    the same keys with the letters a French (AZERTY) keyboard prints on them: the keyboard plays physical
            positions, so L R = A E, left stick = Z Q S D, stick presses = W C. The launcher can serve it only while
            no controller is connected (mods/<version>-keyboard, app/launcher.m).
  xbox      the buttons in the place a controller presses them (controllers follow the Switch's physical layout):
            A B X Y = B A Y X, L R = LB RB, ZL ZR = LT RT, + - = Menu and View, the d-pad as a cross.
  playstation  DualSense / DualShock symbols by position like xbox: A B X Y = Circle Cross Triangle Square,
            L R = L1 R1, ZL ZR = L2 R2, + - = Options and Create, the d-pad as a cross.
  playstation-label  the same, but A B X Y = Cross Circle Square Triangle, for TOTK_FACE_BUTTONS=label or a
            controls.txt with south = a, east = b, west = x, north = y.
The button guide in play is not drawn with this font but from eight textures of the common layout archive (A B X Y, L,
ZL and two left-stick keys); they are redrawn from the same icons (HUD_TEXTURES). Other prompts drawn as fixed images
(some tutorial animations) keep the Switch icons.

Format notes (checked on 1.0.0 and 1.2.1): a .bfotf is an OpenType (CFF) font with an 8-byte header, XOR-ed with a
32-bit key; the first word XOR 0x18029a7f gives the key and the second word, XOR the key, holds the big-endian size.
The archive is a SARC; its members are packed again in their original order and alignment, and every member except
External.bfotf must come out byte-identical. Each archive is compressed with the zstd dictionary it used (Pack/ZsDic).
The redrawn archives are about 1.4 KB larger than the originals, past the few hundred bytes the game's resource size table
leaves them (System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs: a RESTBL, entries keyed by the CRC32
of the path without .zs), and the game stops at start when it loads one. So the script also writes that table into the
mod with each archive's entry at least the original entry plus what the archive grew; a table already in the mod folder
(another mod's, or an earlier run) is the one it edits, so running again changes nothing more.

Needs fontTools (python3.14 -m pip install fonttools) and Python 3.14 (compression.zstd). An existing font archive in the mod
folder (another mod's, or an earlier run) is only overwritten with --replace. The xbox style matches the default face-button
mapping by position (not TOTK_FACE_BUTTONS=label or face buttons changed in controls.txt).
usage: make_button_icons.py <mod folder> (--app <Tears of the Kingdom.app> | --romfs <folder>) [--version v121] [--style xbox] [--replace]
"""
import argparse
import io
import struct
import sys
import zlib
from pathlib import Path

try:
    import compression.zstd as zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd): run it with python3.14')
import make_mod_manifest
from make_mod_manifest import VERSIONS, app_image_dir, merge_build_patches, read_index, write_manifest

try:
    from fontTools.pens.basePen import BasePen
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit('needs fontTools: pip install fonttools')

ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'  # the default archive; the text fonts for the key letters are read from it
FONT = 'scft/External.bfotf'
TEXT_FONTS = ('scft/ninP_RodinNTLG-B_DH_003_subset.bfotf', 'scft/nintendo_NTLG-DB_DH_002.bfotf')
MAGIC = 0x18029a7f


def restbl_offsets(table):
    """CRC32 of a RomFS path without .zs -> offset of its size in the RESTBL `table`."""
    assert table[:6] == b'RESTBL'
    count = struct.unpack_from('<I', table, 14)[0]
    return {struct.unpack_from('<I', table, 22 + i * 8)[0]: 22 + i * 8 + 4 for i in range(count)}


def grow_restbl(table, original, growth):
    """`table` with the entry of each path in `growth` (RomFS path -> bytes) at least its entry in the `original` table
    plus that growth."""
    at, base = restbl_offsets(table), restbl_offsets(original)
    out = bytearray(table)
    for path, extra in growth.items():
        h = zlib.crc32(path.removesuffix('.zs').encode())
        need = struct.unpack_from('<I', original, base[h])[0] + extra
        struct.pack_into('<I', out, at[h], max(struct.unpack_from('<I', out, at[h])[0], need))
    return bytes(out)


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member (as in make_romfs_patches.py)."""
    assert data[:4] == b'SARC' and data[6:8] == b'\xff\xfe'
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    assert size == len(data) and data[header:header + 4] == b'SFAT'
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count * 16
    assert data[names:names + 4] == b'SFNT'; names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out


def bfotf_decode(blob):
    key = struct.unpack_from('<I', blob, 0)[0] ^ MAGIC
    size = int.from_bytes(struct.pack('<I', struct.unpack_from('<I', blob, 4)[0] ^ key), 'big')
    body, out = blob[8:8 + size], bytearray()
    for i in range(0, size - size % 4, 4):
        out += struct.pack('<I', struct.unpack_from('<I', body, i)[0] ^ key)
    return bytes(out + body[size - size % 4:]), key


def bfotf_encode(otf, key):
    size = len(otf)
    out = bytearray(struct.pack('<I', MAGIC ^ key) + struct.pack('<I', struct.unpack('<I', size.to_bytes(4, 'big'))[0] ^ key))
    for i in range(0, size - size % 4, 4):
        out += struct.pack('<I', struct.unpack_from('<I', otf, i)[0] ^ key)
    return bytes(out + otf[size - size % 4:])


def repack(sarc, name, blob):
    """The SARC with member `name` replaced by `blob`, every other member byte-identical, in the original order and alignment."""
    start = struct.unpack_from('<I', sarc, 12)[0]
    fat = 0x14; count = struct.unpack_from('<H', sarc, fat + 6)[0]
    nodes = [list(struct.unpack_from('<4I', sarc, fat + 12 + i * 16)) for i in range(count)]
    members = sarc_members(sarc); by_range = {(lo, hi): k for k, (lo, hi) in members.items()}
    blobs = [blob if by_range[(start + n[2], start + n[3])] == name else sarc[start + n[2]:start + n[3]] for n in nodes]

    def alignment(offset):
        a = 1
        while offset % (a * 2) == 0 and a < 0x2000:
            a *= 2
        return a
    data, placed = bytearray(), {}
    for i in sorted(range(count), key=lambda i: nodes[i][2]):
        a = alignment(nodes[i][2]) if nodes[i][2] else 1
        while len(data) % a:
            data += b'\0'
        placed[i] = (len(data), len(data) + len(blobs[i])); data += blobs[i]
    head = bytearray(sarc[:start])
    for i in range(count):
        struct.pack_into('<II', head, fat + 12 + i * 16 + 8, *placed[i])
    out = bytearray(bytes(head) + bytes(data)); struct.pack_into('<I', out, 8, len(out)); out = bytes(out)
    check = sarc_members(out)
    for k, (lo, hi) in members.items():
        assert out[check[k][0]:check[k][1]] == (blob if k == name else sarc[lo:hi]), k
    return out


# ---- drawing (units of the font's 1000-unit em; the round A/B/X/Y icons fill y 20..585) ----
Y0, Y1 = 20, 585
CY, R = (Y0 + Y1) / 2, (Y1 - Y0) / 2
K = 0.5523


class Glyphs:
    """Outlines of letters from the game's own text fonts, drawn as holes in a shape."""
    def __init__(self, fonts):
        self.fonts = fonts

    def outline(self, ch):
        for font in self.fonts:
            cmap = font.getBestCmap()
            if ord(ch) in cmap:
                glyphs = font.getGlyphSet(); g = glyphs[cmap[ord(ch)]]
                rec = RecordingPen(); g.draw(rec); box = BoundsPen(glyphs); g.draw(box)
                return rec, box.bounds, font['head'].unitsPerEm, g.width
        raise KeyError(f'no font has {ch!r}')

    def hole(self, pen, text, cx, cy, cap, max_width):
        parts, x, ink = [], 0.0, [1e9, 1e9, -1e9, -1e9]
        for ch in text:
            rec, b, upm, advance = self.outline(ch); k = 1000 / upm
            parts.append((rec, k, x))
            ink = [min(ink[0], x + b[0] * k), min(ink[1], b[1] * k), max(ink[2], x + b[2] * k), max(ink[3], b[3] * k)]
            x += advance * k * 0.96
        width, height = ink[2] - ink[0], ink[3] - ink[1]
        scale = min(cap / height, max_width / width)
        ox, oy = cx - (ink[0] + width / 2) * scale, cy - (ink[1] + height / 2) * scale
        reverse = ReverseContourPen(pen)
        for rec, k, x in parts:
            rec.replay(TransformPen(reverse, (scale * k, 0, 0, scale * k, ox + x * scale, oy)))


def circle(pen, cx, cy, r):
    k = K * r
    pen.moveTo((cx + r, cy))
    pen.curveTo((cx + r, cy + k), (cx + k, cy + r), (cx, cy + r))
    pen.curveTo((cx - k, cy + r), (cx - r, cy + k), (cx - r, cy))
    pen.curveTo((cx - r, cy - k), (cx - k, cy - r), (cx, cy - r))
    pen.curveTo((cx + k, cy - r), (cx + r, cy - k), (cx + r, cy))
    pen.closePath()


def rounded(pen, x0, y0, x1, y1, bottom, top=None):
    """Counter-clockwise rounded rectangle (bottom and top corner radii)."""
    top = bottom if top is None else top
    kb, kt = K * bottom, K * top
    pen.moveTo((x0 + bottom, y0)); pen.lineTo((x1 - bottom, y0))
    pen.curveTo((x1 - bottom + kb, y0), (x1, y0 + bottom - kb), (x1, y0 + bottom)); pen.lineTo((x1, y1 - top))
    pen.curveTo((x1, y1 - top + kt), (x1 - top + kt, y1), (x1 - top, y1)); pen.lineTo((x0 + top, y1))
    pen.curveTo((x0 + top - kt, y1), (x0, y1 - top + kt), (x0, y1 - top)); pen.lineTo((x0, y0 + bottom))
    pen.curveTo((x0, y0 + bottom - kb), (x0 + bottom - kb, y0), (x0 + bottom, y0)); pen.closePath()


def polygon(pen, points):
    pen.moveTo(points[0])
    for p in points[1:]:
        pen.lineTo(p)
    pen.closePath()


ARROWS = {'↑': (0, 1), '↓': (0, -1), '←': (-1, 0), '→': (1, 0)}


def arrow_hole(pen, direction, cx, cy, size):
    """An arrow (triangle head and stem) cut out of a key, clockwise so it is a hole."""
    dx, dy = ARROWS[direction]
    def at(u, v):   # u along the arrow, v across it
        return (cx + u * dx * size - v * dy * size, cy + u * dy * size + v * dx * size)
    pts = [at(0.5, 0), at(0.0, 0.42), at(0.0, 0.15), at(-0.5, 0.15), at(-0.5, -0.15), at(0.0, -0.15), at(0.0, -0.42)]
    polygon(ReverseContourPen(pen), pts)


def key(glyphs, pen, x0, y0, x1, y1, label, radius=None, cap=None):
    w, h = x1 - x0, y1 - y0
    rounded(pen, x0, y0, x1, y1, radius if radius is not None else min(w, h) * 0.2)
    if label in ARROWS:
        arrow_hole(pen, label, (x0 + x1) / 2, (y0 + y1) / 2, min(w, h) * 0.62)
    elif label:
        glyphs.hole(pen, label, (x0 + x1) / 2, (y0 + y1) / 2, cap or h * 0.56, w * 0.78)


def keyboard_style(g, layout=None):
    def k(label):   # the letter another layout prints on the same physical key
        return label.translate(layout) if layout and len(label) == 1 else label
    def single(label, advance=640):
        return advance, lambda pen: key(g, pen, 40, Y0, advance - 40, Y1, k(label))
    def wide(label, advance):
        return advance, lambda pen: key(g, pen, 40, Y0 + 40, advance - 40, Y1 - 40, label, radius=110, cap=230)
    def pair(a, b, advance=960):
        def draw(pen):
            w = (advance - 80 - 50) / 2
            key(g, pen, 40, Y0 + 60, 40 + w, Y1 - 60, k(a)); key(g, pen, 40 + w + 50, Y0 + 60, advance - 40, Y1 - 60, k(b))
        return advance, draw
    def cluster(top, left, mid, right, advance=960):
        def draw(pen):
            gap = 24; w = (advance - 80 - 2 * gap) / 3; h = (Y1 + 40 - (Y0 - 40) - gap) / 2
            bottom = Y0 - 40; upper = bottom + h + gap; xs = [40, 40 + w + gap, 40 + 2 * (w + gap)]
            for label, (x, y) in ((top, (xs[1], upper)), (left, (xs[0], bottom)), (mid, (xs[1], bottom)), (right, (xs[2], bottom))):
                key(g, pen, x, y, x + w, y + h, k(label), radius=w * 0.18, cap=h * 0.6)
        return advance, draw
    return {
        0xE040: single('J'), 0xE041: single('K'), 0xE042: single('I'), 0xE043: single('U'),
        0xE044: single('Q', 720), 0xE045: single('E', 720), 0xE046: wide('Opt', 900), 0xE047: wide('Shift', 1080),
        0xE049: cluster('T', 'F', 'G', 'H', 900), 0xE04B: wide('Enter', 1080), 0xE04C: wide('Tab', 820),
        # left stick: W A S D
        0xE050: single('S'), 0xE052: single('W'), 0xE054: pair('A', 'D'), 0xE058: single('D'), 0xE05A: single('A'),
        0xE05C: pair('W', 'S'), 0xE05E: cluster('W', 'A', 'S', 'D'), 0xE056: single('Z'),
        # right stick: arrow keys
        0xE051: single('↓'), 0xE053: single('↑'), 0xE055: pair('←', '→'), 0xE059: single('→'), 0xE05B: single('←'),
        0xE05D: pair('↑', '↓'), 0xE05F: cluster('↑', '←', '↓', '→'), 0xE068: cluster('↑', '←', '↓', '→'), 0xE057: single('C'),
        # d-pad: T F G H
        0xE060: single('T'), 0xE061: single('G'), 0xE062: single('F'), 0xE063: single('H'),
        0xE064: pair('T', 'G', 900), 0xE065: pair('F', 'H', 900),
    }


def stick(g, label, up=False, advance=640):
    """The left stick as a controller's own label in a circle (L3 on a DualSense, LS on an Xbox pad); up: with an arrow up,
    the button guide's "push it forward" (Nt_KeyTexLSt_00, U+E052). The Xbox and PlayStation styles need these too: the
    HUD redraw takes every texture of HUD_TEXTURES from the style."""
    def draw(pen):
        cx = advance / 2; circle(pen, cx, CY, R)
        if up:
            g.hole(pen, label, cx, CY - R * 0.25, R * 0.55, R * 1.1); arrow_hole(pen, '↑', cx, CY + R * 0.5, R * 0.6)
        else:
            g.hole(pen, label, cx, CY, R * 0.95, R * 1.3)
    return advance, draw


def xbox_style(g):
    def face(letter, advance=640):
        def draw(pen):
            circle(pen, advance / 2, CY, R); g.hole(pen, letter, advance / 2, CY, R * 0.95, R * 1.3)
        return advance, draw
    def bumper(label, advance=820):
        def draw(pen):
            rounded(pen, 40, Y0 + 70, advance - 40, Y1 - 70, 70, 150); g.hole(pen, label, advance / 2, CY, 230, advance - 200)
        return advance, draw
    def trigger(label, advance=760):
        def draw(pen):
            rounded(pen, 60, Y0, advance - 60, Y1, 60, 230); g.hole(pen, label, advance / 2, CY - 20, 230, advance - 220)
        return advance, draw
    def menu(advance=640):
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen)
            for dy in (-115, 0, 115):
                rounded(reverse, advance / 2 - 150, CY + dy - 38, advance / 2 + 150, CY + dy + 38, 38)
        return advance, draw
    def view(advance=640):
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen); s = 150
            rounded(reverse, advance / 2 - s - 15, CY + 15, advance / 2 - 15, CY + s + 15, 16)
            rounded(reverse, advance / 2 + 15, CY - s - 15, advance / 2 + s + 15, CY - 15, 16)
        return advance, draw
    def dpad(*lit, advance=640):
        def draw(pen):
            cx, length, arm = advance / 2, R, R * 0.40
            polygon(pen, [(cx + length, CY - arm), (cx + length, CY + arm), (cx + arm, CY + arm), (cx + arm, CY + length),
                          (cx - arm, CY + length), (cx - arm, CY + arm), (cx - length, CY + arm), (cx - length, CY - arm),
                          (cx - arm, CY - arm), (cx - arm, CY - length), (cx + arm, CY - length), (cx + arm, CY - arm)])
            m, t, h = (length + arm) / 2, arm * 0.72, arm * 0.80   # arm centre, half base and height of the arrow
            heads = {'up': [(cx - t, CY + m - h / 2), (cx, CY + m + h / 2), (cx + t, CY + m - h / 2)],
                     'down': [(cx + t, CY - m + h / 2), (cx, CY - m - h / 2), (cx - t, CY - m + h / 2)],
                     'left': [(cx - m + h / 2, CY - t), (cx - m - h / 2, CY), (cx - m + h / 2, CY + t)],
                     'right': [(cx + m - h / 2, CY + t), (cx + m + h / 2, CY), (cx + m - h / 2, CY - t)]}
            for d in lit:
                polygon(pen, heads[d])   # already clockwise: a hole
        return advance, draw
    return {
        0xE040: face('B'), 0xE041: face('A'), 0xE042: face('Y'), 0xE043: face('X'),
        0xE044: bumper('LB'), 0xE045: bumper('RB'), 0xE046: trigger('LT'), 0xE047: trigger('RT'),
        0xE04B: menu(), 0xE04C: view(),
        0xE049: dpad('up', 'down', 'left', 'right'), 0xE060: dpad('up'), 0xE061: dpad('down'),
        0xE062: dpad('left'), 0xE063: dpad('right'), 0xE064: dpad('up', 'down'), 0xE065: dpad('left', 'right'),
        0xE052: stick(g, 'LS', up=True), 0xE05E: stick(g, 'LS'),
    }


def oriented(pen, points, hole):
    """A polygon counter-clockwise (filled) or clockwise (a hole), whatever order the points come in."""
    area = sum(x0 * y1 - x1 * y0 for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]))
    polygon(pen, points if (area > 0) != hole else points[::-1])


def playstation_style(g, by_label=False):
    """DualSense / DualShock symbols. By position (the default mapping): A B X Y = Circle Cross Triangle Square.
    by_label (TOTK_FACE_BUTTONS=label, or controls.txt south = a, east = b, west = x, north = y): Cross Circle Square Triangle."""
    def face(symbol, advance=640):
        def draw(pen):
            cx, s = advance / 2, R * 0.50   # symbol half size
            circle(pen, cx, CY, R)
            if symbol == 'circle':
                circle(ReverseContourPen(pen), cx, CY, s); circle(pen, cx, CY, s * 0.70)
            elif symbol == 'square':
                oriented(pen, [(cx - s, CY - s), (cx + s, CY - s), (cx + s, CY + s), (cx - s, CY + s)], hole=True)
                t = s * 0.70
                oriented(pen, [(cx - t, CY - t), (cx + t, CY - t), (cx + t, CY + t), (cx - t, CY + t)], hole=False)
            elif symbol == 'triangle':
                pts = [(cx, CY + s * 1.10), (cx - s * 1.10, CY - s * 0.75), (cx + s * 1.10, CY - s * 0.75)]
                gx, gy = cx, sum(y for _, y in pts) / 3
                oriented(pen, pts, hole=True)
                oriented(pen, [(gx + (x - gx) * 0.62, gy + (y - gy) * 0.62) for x, y in pts], hole=False)
            else:   # cross: one 12-point outline, so the two strokes do not overlap as separate holes
                a, w = s * 1.05, s * 0.22 * 2 ** 0.5   # arm reach along the diagonal, half stroke width
                pts = [(cx, CY + w), (cx + a - w, CY + a), (cx + a, CY + a - w), (cx + w, CY),
                       (cx + a, CY - a + w), (cx + a - w, CY - a), (cx, CY - w), (cx - a + w, CY - a),
                       (cx - a, CY - a + w), (cx - w, CY), (cx - a, CY + a - w), (cx - a + w, CY + a)]
                oriented(pen, pts, hole=True)
        return advance, draw
    def bumper(label, advance=820):
        def draw(pen):
            rounded(pen, 40, Y0 + 70, advance - 40, Y1 - 70, 70, 150); g.hole(pen, label, advance / 2, CY, 230, advance - 200)
        return advance, draw
    def trigger(label, advance=760):
        def draw(pen):
            rounded(pen, 60, Y0, advance - 60, Y1, 60, 230); g.hole(pen, label, advance / 2, CY - 20, 230, advance - 220)
        return advance, draw
    def options(advance=640):   # three bars, as on the DualSense Options button
        def draw(pen):
            circle(pen, advance / 2, CY, R); reverse = ReverseContourPen(pen)
            for dy in (-115, 0, 115):
                rounded(reverse, advance / 2 - 150, CY + dy - 38, advance / 2 + 150, CY + dy + 38, 38)
        return advance, draw
    def create(advance=640):   # three short strokes rising to the right, as on the Create button
        def draw(pen):
            circle(pen, advance / 2, CY, R); cx = advance / 2
            for dx in (-110, 0, 110):
                oriented(pen, [(cx + dx - 30, CY - 150), (cx + dx + 30, CY - 150), (cx + dx + 30, CY + 150), (cx + dx - 30, CY + 150)], hole=True)
        return advance, draw
    dpad_style = xbox_style(g)
    a, b, x, y = ('cross', 'circle', 'square', 'triangle') if by_label else ('circle', 'cross', 'triangle', 'square')
    icons = {
        0xE040: face(a), 0xE041: face(b), 0xE042: face(x), 0xE043: face(y),
        0xE044: bumper('L1'), 0xE045: bumper('R1'), 0xE046: trigger('L2'), 0xE047: trigger('R2'),
        0xE04B: options(), 0xE04C: create(),
        0xE052: stick(g, 'L3', up=True), 0xE05E: stick(g, 'L3'),
    }
    for cp in (0xE049, 0xE060, 0xE061, 0xE062, 0xE063, 0xE064, 0xE065):   # the d-pad is drawn as for Xbox
        icons[cp] = dpad_style[cp]
    return icons


AZERTY = str.maketrans('QWAZ', 'AZQW')   # US key position -> the letter an AZERTY keyboard prints there

STYLES = {'keyboard': keyboard_style, 'azerty': lambda g: keyboard_style(g, AZERTY), 'xbox': xbox_style, 'playstation': playstation_style,
          'playstation-label': lambda g: playstation_style(g, by_label=True)}


# ---- the HUD's button guide: images, not glyphs ----
# The button guide in play (bottom right) draws its keys from textures of the common layout archive, not from the font:
# Square R8_UNORM masks (white key, black letter; 72x72 up to 1.2.1, 108x108 in 1.4.3), block-linear, one mip, in timg/__Combined.bntx. Each is redrawn from the
# style's icon for the same Switch button, fitted into the same square, and written back in place: the BNTX keeps its size.
LAYOUT_PREFIX, BNTX = 'UI/LayoutArchive/Common.', 'timg/__Combined.bntx'
HUD_TEXTURES = {'Nt_KeyTexA_00': 0xE040, 'Nt_KeyTexB_00': 0xE041, 'Nt_KeyTexX_00': 0xE042, 'Nt_KeyTexY_00': 0xE043,
                'Nt_KeyTexL_00': 0xE044, 'Nt_KeyTexZL_00': 0xE046,
                'Nt_KeyTexLSt_00': 0xE052,   # the left stick with an arrow up: push it forward
                'Nt_KeyTexLSt_01': 0xE05E}   # the left stick
R8_UNORM, HUD_MARGIN, SUPERSAMPLE = 0x201, 4, 4


def gob_offset(x, y, width_gobs, block_height):
    """Byte offset of (x, y) in a Tegra block-linear surface of 1 byte per pixel (GOBs of 64 bytes x 8 rows)."""
    return ((y // (8 * block_height)) * 512 * block_height * width_gobs + (x // 64) * 512 * block_height
            + ((y % (8 * block_height)) // 8) * 512 + ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64
            + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16))


class PolygonPen(BasePen):
    """Outlines flattened to closed polygons."""
    def __init__(self):
        super().__init__(None); self.polygons = []

    def _moveTo(self, pt):
        self.polygons.append([pt])

    def _lineTo(self, pt):
        self.polygons[-1].append(pt)

    def _curveToOne(self, pt1, pt2, pt3):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u ** 3 * p0[k] + 3 * u * u * t * pt1[k] + 3 * u * t * t * pt2[k] + t ** 3 * pt3[k] for k in (0, 1)))

    def _qCurveToOne(self, pt1, pt2):
        p0 = self.polygons[-1][-1]
        for i in range(1, 17):
            t = i / 16; u = 1 - t
            self.polygons[-1].append(tuple(u * u * p0[k] + 2 * u * t * pt1[k] + t * t * pt2[k] for k in (0, 1)))


def rasterize(draw, size):
    """A size x size coverage mask (0..255, rows top down) of `draw`'s outline, fitted into the square, nonzero winding."""
    rec = RecordingPen(); draw(rec)
    box = BoundsPen(None); rec.replay(box); x0, y0, x1, y1 = box.bounds
    inner = size - 2 * HUD_MARGIN; scale = inner / max(x1 - x0, y1 - y0)
    ox = HUD_MARGIN + (inner - (x1 - x0) * scale) / 2 - x0 * scale
    oy = HUD_MARGIN + (inner - (y1 - y0) * scale) / 2 - y0 * scale
    flat = PolygonPen(); rec.replay(TransformPen(flat, (scale, 0, 0, -scale, ox, size - oy)))   # font y up -> rows down
    edges = [(a, b) for poly in flat.polygons for a, b in zip(poly, poly[1:] + poly[:1]) if a[1] != b[1]]
    n = size * SUPERSAMPLE; cover = [0] * (size * size)
    for row in range(n):
        y = (row + 0.5) / SUPERSAMPLE
        crossings = sorted((a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]), 1 if b[1] > a[1] else -1)
                           for a, b in edges if min(a[1], b[1]) <= y < max(a[1], b[1]))
        winding, start = 0, None
        for x, d in crossings:
            if winding == 0:
                start = x
            winding += d
            if winding == 0:
                for col in range(max(0, round(start * SUPERSAMPLE)), min(n, round(x * SUPERSAMPLE))):
                    cover[(row // SUPERSAMPLE) * size + col // SUPERSAMPLE] += 1
    return bytes(min(255, c * 255 // (SUPERSAMPLE * SUPERSAMPLE)) for c in cover)


def redraw_hud(bntx, icons):
    """The BNTX with every texture of HUD_TEXTURES redrawn from `icons` (code point -> (advance, draw)), same size."""
    out, done, pos = bytearray(bntx), [], 0
    while (i := bntx.find(b'BRTI', pos)) >= 0:
        pos = i + 4
        name_at = struct.unpack_from('<Q', bntx, i + 0x60)[0]; length = struct.unpack_from('<H', bntx, name_at)[0]
        name = bntx[name_at + 2:name_at + 2 + length].decode().split('^')[0]
        if name not in HUD_TEXTURES:
            continue
        mips, fmt, (w, h), layout = (struct.unpack_from('<H', bntx, i + 0x16)[0], struct.unpack_from('<I', bntx, i + 0x1C)[0],
                                     struct.unpack_from('<II', bntx, i + 0x24), struct.unpack_from('<I', bntx, i + 0x34)[0])
        assert fmt == R8_UNORM and w == h and mips == 1, f'{name}: format {fmt:#x}, {w}x{h}, {mips} mips'   # 72x72 to 1.2.1, 108x108 in 1.4.3
        data = struct.unpack_from('<Q', bntx, struct.unpack_from('<Q', bntx, i + 0x70)[0])[0]
        mask = rasterize(icons[HUD_TEXTURES[name]][1], w)
        for y in range(h):
            for x in range(w):
                out[data + gob_offset(x, y, (w + 63) // 64, 1 << (layout & 7))] = mask[y * w + x]
        done.append(name)
    missing = set(HUD_TEXTURES) - set(done)
    assert not missing, f'not in the layout archive: {", ".join(sorted(missing))}'
    return bytes(out)


def redraw(otf, text_fonts, style):
    font = TTFont(io.BytesIO(otf))
    cff = font['CFF '].cff; top = cff.topDictIndex[0]; charstrings = top.CharStrings; private = top.Private
    nominal = getattr(private, 'nominalWidthX', 0); cmap = font.getBestCmap()
    icons = STYLES[style](Glyphs(text_fonts))
    for cp, (advance, draw) in icons.items():
        pen = T2CharStringPen(advance - nominal, None); draw(pen)
        charstrings[cmap[cp]] = pen.getCharString(private=private, globalSubrs=cff.GlobalSubrs)
        font['hmtx'][cmap[cp]] = (advance, 0)
    glyphs = font.getGlyphSet()
    for cp, (advance, _) in icons.items():
        box = BoundsPen(glyphs); glyphs[cmap[cp]].draw(box)
        font['hmtx'][cmap[cp]] = (advance, int(round(box.bounds[0])))
    out = io.BytesIO(); font.save(out)
    return out.getvalue(), len(icons)


def write_mod_manifest(mod, romfs, index):
    """Merge the mod's Common archive with the build's own patch of it (make_romfs_patches.py patches the same archive),
    then write the manifest: (merged paths, Manifest). The merged file's line names the build's file as the original, so
    the game loads it in place of the build's overlay; without the merge both replace one file and the game stops at start."""
    merged = merge_build_patches(mod, romfs, index)
    return merged, write_manifest(mod, romfs, merged=merged)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder to write into (other files in it are kept and listed too)')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path, help='the built Tears of the Kingdom.app')
    source.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and runtime-index.bin')
    parser.add_argument('--romfs-data', type=Path, help='the folder holding romfs.bin when it is not beside the index (an app '
                        'built with --mac-game-data library: <data>/GameData/<version>; with --app that folder is the default)')
    parser.add_argument('--version', choices=VERSIONS, help='the game version (default: the mod folder name)')
    parser.add_argument('--style', choices=sorted(STYLES), default='keyboard')
    parser.add_argument('--replace', action='store_true', help='overwrite a font archive already in the mod folder')
    args = parser.parse_args()
    version = args.version or (args.mod.name if args.mod.name in VERSIONS else None)
    romfs = args.romfs
    if args.app:
        if not version:
            parser.error('name the game version with --version (or name the mod folder v0, v112, v121 or v143)')
        romfs = args.app / f'Contents/Resources/totk/extracted/{version}/romfs'
    image_dir = args.romfs_data or romfs
    if args.app and not args.romfs_data and not (romfs / 'romfs.bin').exists():   # built with --mac-game-data library
        image_dir = app_image_dir(args.app, version)
    make_mod_manifest.IMAGE_DIR = image_dir   # write_manifest reads the originals from the image too
    for need in (image_dir / 'romfs.bin', romfs / 'runtime-index.bin'):
        if not need.is_file():
            parser.error(f'{need} not found')

    index = read_index(romfs / 'runtime-index.bin')
    archives = [ARCHIVE] + sorted(p for p in index if p.startswith('Font/') and p.endswith('.bfarc.zs') and p != ARCHIVE)
    layout_path = next((p for p in index if p.startswith(LAYOUT_PREFIX) and p.endswith('.blarc.zs')), None)
    if not layout_path:
        parser.error(f'{romfs} holds no {LAYOUT_PREFIX}*.blarc.zs (the in-game button guide): is this Tears of the Kingdom\'s RomFS?')
    restbl = next((p for p in index if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.rsizetable.zs')), None)
    if not restbl:
        parser.error(f'{romfs} holds no System/Resource/ResourceSizeTable.*.rsizetable.zs: is this Tears of the Kingdom\'s RomFS?')
    existing = [str(args.mod / p) for p in archives + [layout_path] if (args.mod / p).exists()]
    if existing and not args.replace:
        parser.error(f'{", ".join(existing)} already exist(s) (another mod\'s font archive, or an earlier run): '
                     'pass --replace to overwrite')
    with (image_dir / 'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); data = image.read(size); assert len(data) == size; return data
        dictionaries = {}
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        for lo, hi in sarc_members(pack).values():
            d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        raws = {path: read(path) for path in archives}
        restbl_raw = read(restbl)
        layout = layout_path
        layout_raw = read(layout)

    text_fonts, redrawn, growth = None, {}, {}
    for path in archives:
        info = zstd.get_frame_info(raws[path]); dictionary = dictionaries[info.dictionary_id]
        archive = zstd.decompress(raws[path], zstd_dict=dictionary); assert len(archive) == info.decompressed_size
        members = sarc_members(archive)
        if FONT not in members:
            continue
        if text_fonts is None:
            text_fonts = [TTFont(io.BytesIO(bfotf_decode(archive[slice(*members[name])])[0])) for name in TEXT_FONTS]
        otf, key = bfotf_decode(archive[slice(*members[FONT])])
        assert otf[:4] == b'OTTO', f'External.bfotf in {path} did not decode to an OpenType font'
        if otf not in redrawn:
            redrawn[otf] = redraw(otf, text_fonts, args.style)
        new_otf, count = redrawn[otf]
        new_blob = bfotf_encode(new_otf, key); assert bfotf_decode(new_blob)[0] == new_otf
        new_archive = repack(archive, FONT, new_blob)
        data = zstd.compress(new_archive, level=19, zstd_dict=dictionary)
        assert zstd.decompress(data, zstd_dict=dictionary) == new_archive
        assert zstd.get_frame_info(data).dictionary_id == info.dictionary_id

        growth[path] = len(new_archive) - len(archive)
        target = args.mod / path; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
        print(f'{target}: {count} icons redrawn ({args.style}), {len(data)} bytes')

    info = zstd.get_frame_info(layout_raw); dictionary = dictionaries[info.dictionary_id]
    archive = zstd.decompress(layout_raw, zstd_dict=dictionary); members = sarc_members(archive)
    new_archive = repack(archive, BNTX, redraw_hud(archive[slice(*members[BNTX])], STYLES[args.style](Glyphs(text_fonts))))
    assert len(new_archive) == len(archive)
    data = zstd.compress(new_archive, level=19, zstd_dict=dictionary)
    assert zstd.decompress(data, zstd_dict=dictionary) == new_archive
    growth[layout] = 0
    target = args.mod / layout; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    print(f'{target}: {len(HUD_TEXTURES)} button guide textures redrawn ({args.style}), {len(data)} bytes')

    target = args.mod / restbl
    original = zstd.decompress(restbl_raw)
    table = zstd.decompress(target.read_bytes()) if target.exists() else original
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(zstd.compress(grow_restbl(table, original, growth), level=19))
    print(f'{target}: ' + ', '.join(f'{p} {g:+d}' for p, g in growth.items()))
    try:
        merged, found = write_mod_manifest(args.mod, romfs, index)
    except ValueError as error:
        sys.exit(f'manifest not written: {error}')
    for name in merged:
        print(f'{args.mod / name}: merged with the build\'s own patch of it')
    if found.misspelt:
        sys.exit('manifest not written: ' + ', '.join(f'{a} (the RomFS has {b})' for a, b in found.misspelt))
    print(f'{args.mod / "manifest.tsv"}: {len(found.lines)} files '
          f'({len(found.lines) - len(found.added)} replaced, {len(found.added)} added)')


if __name__ == '__main__':
    main()
