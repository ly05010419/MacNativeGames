#!/usr/bin/env python3
"""Make a RomFS mod that draws keyboard keys or Xbox controller buttons in place of the Switch button icons.

The A/B/X/Y, L/R, ZL/ZR, +/- and d-pad icons the game shows in its prompts are glyphs of one font,
scft/External.bfotf in Font/Font.Nin_NX_NVN.bfarc.zs (private-use code points U+E040 to U+E068). This script reads that
archive from the player's own RomFS, redraws those glyphs, packs the archive back the way the game stores it and writes
it into a mod folder (runtime/romfs.c serves it in place of the original), then writes the folder's manifest.tsv with
make_mod_manifest.py. Nothing of the game ships with the project: every font comes from the player's RomFS. The archive
is the same in 1.0.0 and 1.2.1 (same SHA-256), so one mod folder per version is enough.

Styles:
  keyboard  the keys the Mac keyboard plays (app/launcher.m): A B X Y = J K I U, L R = Q E, ZL ZR = Option Shift,
            + - = Enter Tab, left stick = W A S D, right stick = arrow keys, stick presses = Z C, d-pad = T F G H.
  xbox      the buttons in the place a controller presses them (controllers follow the Switch's physical layout):
            A B X Y = B A Y X, L R = LB RB, ZL ZR = LT RT, + - = Menu and View, the d-pad as a cross.
Prompts drawn as fixed images (some tutorial animations) keep the Switch icons: they are not glyphs of this font.

Format notes (checked on 1.0.0 and 1.2.1): a .bfotf is an OpenType (CFF) font with an 8-byte header, XOR-ed with a
32-bit key; the first word XOR 0x18029a7f gives the key and the second word, XOR the key, holds the big-endian size.
The archive is a SARC; its members are packed again in their original order and alignment, and every member except
External.bfotf must come out byte-identical. The archive is compressed with the zstd dictionary it used (Pack/ZsDic).

Needs fontTools (python3.14 -m pip install fonttools) and Python 3.14 (compression.zstd). An existing font archive in the mod
folder (another mod's, or an earlier run) is only overwritten with --replace. The xbox style matches the default face-button
mapping by position (not TOTK_FACE_BUTTONS=label or face buttons changed in controls.txt).
usage: make_button_icons.py <mod folder> (--app <Tears of the Kingdom.app> | --romfs <folder>) [--version v121] [--style xbox] [--replace]
"""
import argparse
import io
import struct
import sys
from pathlib import Path

try:
    import compression.zstd as zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd): run it with python3.14')
from make_mod_manifest import VERSIONS, manifest, read_index

try:
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen
    from fontTools.pens.t2CharStringPen import T2CharStringPen
    from fontTools.pens.transformPen import TransformPen
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit('needs fontTools: pip install fonttools')

ARCHIVE = 'Font/Font.Nin_NX_NVN.bfarc.zs'
FONT = 'scft/External.bfotf'
TEXT_FONTS = ('scft/ninP_RodinNTLG-B_DH_003_subset.bfotf', 'scft/nintendo_NTLG-DB_DH_002.bfotf')
MAGIC = 0x18029a7f


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


def keyboard_style(g):
    def single(label, advance=640):
        return advance, lambda pen: key(g, pen, 40, Y0, advance - 40, Y1, label)
    def wide(label, advance):
        return advance, lambda pen: key(g, pen, 40, Y0 + 40, advance - 40, Y1 - 40, label, radius=110, cap=230)
    def pair(a, b, advance=960):
        def draw(pen):
            w = (advance - 80 - 50) / 2
            key(g, pen, 40, Y0 + 60, 40 + w, Y1 - 60, a); key(g, pen, 40 + w + 50, Y0 + 60, advance - 40, Y1 - 60, b)
        return advance, draw
    def cluster(top, left, mid, right, advance=960):
        def draw(pen):
            gap = 24; w = (advance - 80 - 2 * gap) / 3; h = (Y1 + 40 - (Y0 - 40) - gap) / 2
            bottom = Y0 - 40; upper = bottom + h + gap; xs = [40, 40 + w + gap, 40 + 2 * (w + gap)]
            for label, (x, y) in ((top, (xs[1], upper)), (left, (xs[0], bottom)), (mid, (xs[1], bottom)), (right, (xs[2], bottom))):
                key(g, pen, x, y, x + w, y + h, label, radius=w * 0.18, cap=h * 0.6)
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
    }


STYLES = {'keyboard': keyboard_style, 'xbox': xbox_style}


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


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder to write into (other files in it are kept and listed too)')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path, help='the built Tears of the Kingdom.app')
    source.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and runtime-index.bin')
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
    if (args.mod / ARCHIVE).exists() and not args.replace:
        parser.error(f'{args.mod / ARCHIVE} already exists (another mod\'s font archive, or an earlier run): '
                     'pass --replace to overwrite it')
    for need in ('romfs.bin', 'runtime-index.bin'):
        if not (romfs / need).is_file():
            parser.error(f'{romfs / need} not found')

    index = read_index(romfs / 'runtime-index.bin')
    with (romfs / 'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); data = image.read(size); assert len(data) == size; return data
        dictionaries = {}
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        for lo, hi in sarc_members(pack).values():
            d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        raw = read(ARCHIVE)
    info = zstd.get_frame_info(raw); dictionary = dictionaries[info.dictionary_id]
    archive = zstd.decompress(raw, zstd_dict=dictionary); assert len(archive) == info.decompressed_size
    members = sarc_members(archive)
    otf, key = bfotf_decode(archive[slice(*members[FONT])])
    assert otf[:4] == b'OTTO', 'External.bfotf did not decode to an OpenType font'
    text_fonts = [TTFont(io.BytesIO(bfotf_decode(archive[slice(*members[name])])[0])) for name in TEXT_FONTS]
    new_otf, count = redraw(otf, text_fonts, args.style)
    new_blob = bfotf_encode(new_otf, key); assert bfotf_decode(new_blob)[0] == new_otf
    new_archive = repack(archive, FONT, new_blob)
    data = zstd.compress(new_archive, level=19, zstd_dict=dictionary)
    assert zstd.decompress(data, zstd_dict=dictionary) == new_archive
    assert zstd.get_frame_info(data).dictionary_id == info.dictionary_id

    target = args.mod / ARCHIVE; target.parent.mkdir(parents=True, exist_ok=True); target.write_bytes(data)
    print(f'{target}: {count} icons redrawn ({args.style}), {len(data)} bytes')
    lines, _, added, misspelt = manifest(args.mod, romfs)
    if misspelt:
        sys.exit('manifest not written: ' + ', '.join(f'{a} (the RomFS has {b})' for a, b in misspelt))
    (args.mod / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_mod_manifest.py)\n'
                                           + '\n'.join(lines) + '\n')
    print(f'{args.mod / "manifest.tsv"}: {len(lines)} files ({len(lines) - len(added)} replaced, {len(added)} added)')


if __name__ == '__main__':
    main()
