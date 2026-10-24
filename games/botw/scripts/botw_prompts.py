#!/usr/bin/env python3
"""Make a RomFS mod that shows Xbox or PlayStation button names in Breath of the Wild's on-screen prompts, for the buttons you really press.

The game draws its main prompts ("A: Talk") as 48x48 textures of the button, one texture for each of A, B, X, Y, L, ZL and
the left stick press, in Pack/Bootup.pack > Layout/Common.sblarc > timg/__Combined.bntx. This tool reads that archive from
your own RomFS and, for each Switch button, shows the Xbox (or PlayStation) button you press for it (the chain of
prompt_layout.py: face buttons by position or label, then your controls.txt). Every button is drawn again from one drawing of
make_button_icons.py (A B X Y as discs, L ZL as the LB LT keys, Menu and View, the d-pad cross) and fitted on the place of the
game's own picture: the disc of the face buttons and the d-pad, the box of the white shape of the L and ZL keys. With --style
playstation the keys read L1 L2 R1 R2 and the faces are the Cross, Circle, Square and Triangle. A stick keeps the game's own
picture in every style (a stick with a small L or R on it): a stick press prompt shows the game's picture of the stick pressed,
traced as the switch style traces it. With --style switch every picture is the game's own,
traced (sharp at a larger --scale, the same look) from the game's picture of the button pressed (controls.txt); a texture with
no picture of that button gets a drawn Switch key. At --scale 2 and up every other button glyph of the font (a stick being
moved, a button nothing drives) is traced too, in every style.
Letters are drawn from the Switch's own system font, which your Breath of the Wild app already holds (shared-fonts/
0-standard.bfttf). Nothing of the game ships with the project: every picture comes from your files, and the result is written
only into the mod folder you name (then manifest.tsv, made with make_mod_manifest.py; runtime/romfs.c checks it at start).
The icons inside text (controller tips, the pick-up window, camera mode) are glyphs of the button font in Font/Font_*.sbfarc
(External_00.bffnt, a BC4 glyph sheet): the same drawings are used there for A B X Y, L R ZL ZR (as LB RB LT RT), + and -
(as the Menu and View buttons, or Options and Create) and the d-pad glyphs (four dots become a d-pad cross), as does the d-pad
of the shortcut icons on the screen (ButtonIconL46NN_00); the two stick press glyphs are the game's, of the stick pressed. A
button's texture and its font glyph therefore look the same. The glyphs of a stick being moved stay the game's.
The controller pictures of the controller tips page are drawn again too: the handheld Switch (Nt_NNillust_00, _01) and the four pieces
of the hint boxes (Nt_GamePadIllustSnn_00 to _03) become an Xbox controller (or with --style playstation a DualSense) drawn by
botw_controller_art.py, with the buttons in the places of the Switch's, so the game's callout lines still end on a button of the same
kind. --keep-switch-pictures leaves them alone; --style switch always does (the hint box rings still go to the button pressed).
--scale 2 to 4 draws every picture that many times as large (sharp on a big screen): the textures and the whole glyph sheet of
every font grow (the unchanged glyphs are enlarged by repeating their pixels), the layout and font archives get bigger, and
the resource size table (System/Resource/ResourceSizeTable.product.srsizetable) is written into the mod too, with the growth
added to the entries of Layout/Common.blarc and Font/Font_*.bfarc. Scale 1 (the default) keeps every picture at the game's size.
--mods names the player's mod folder (the launcher passes it): when its manifest.tsv replaces the resource size table (an HD
texture pack ships one), that table is the one raised, each entry to the larger of the mod's size and the game's grown one, and
the manifest names it as the original, so runtime/romfs.c loads these icons over the mod's table instead of leaving them out.

Another game version may differ from 1.9.0: a texture that is missing or not the expected format or size, or a font file with no
External_00.bffnt or an unexpected glyph sheet, is skipped with a note and stays the game's. With --scale above 1, a resource size
table that is missing or lacks an entry for an archive that grows (or an archive that cannot grow) makes the whole run draw at
--scale 1. A RomFS without Pack/Bootup.pack, Layout/Common.sblarc or its BNTX stops the run (exit 2) before anything is written.

Needs Python 3.14 and fontTools (python3.14 -m pip install fonttools). An existing Pack/Bootup.pack (with --scale above 1 also the
resource size table) in the mod folder (another mod's, or an earlier run) is only overwritten with --replace.
usage: botw_prompts.py <mod folder> (--app <Breath of the Wild.app> | --romfs <folder>) [--version botw190]
                       [--style xbox|playstation|switch] [--face-buttons position|label] [--controls controls.txt] [--replace] [--keep-switch-pictures] [--scale 1..4] [--mods <folder>] [--preview picture.png]
"""
import argparse
import functools
import hashlib
import io
import math
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import botw_formats as bf                                                        # noqa: E402
import botw_controller_art as art                                                # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'totk' / 'scripts'))
from make_mod_manifest import APP_VERSIONS, read_index, write_manifest          # noqa: E402
from prompt_layout import FACE_MODES, LayoutError, describe, prompt_controls    # noqa: E402

PACK, LAYOUT, BNTX = 'Pack/Bootup.pack', 'Layout/Common.sblarc', 'timg/__Combined.bntx'
FACE = {slot: f'Nt_KeyTex{slot.upper()}_00^d' for slot in 'abxy'}               # Switch button -> its texture
SHAPE = {'l': ['Nt_KeyTexL_00^d'], 'zl': ['Nt_KeyTexZL_00^d'], 'lstick': ['Nt_KeyTexLSt_00^d', 'Nt_KeyTexLSt_01^d']}
XBOX_TEXT = {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y', 'l1': 'LB', 'r1': 'RB', 'l2': 'LT', 'r2': 'RT', 'l3': 'LS', 'r3': 'RS'}
PICTURES = art.PICTURES      # the controller pictures of the controller tips page and the hint boxes
PLAYSTATION_TEXT = {'a': 'Cross', 'b': 'Circle', 'x': 'Square', 'y': 'Triangle', 'l1': 'L1', 'r1': 'R1', 'l2': 'L2', 'r2': 'R2', 'l3': 'L3', 'r3': 'R3'}
SWITCH_TEXT = {'a': 'A', 'b': 'B', 'x': 'X', 'y': 'Y', 'l1': 'L', 'r1': 'R', 'l2': 'ZL', 'r2': 'ZR', 'l3': 'L', 'r3': 'R'}
STYLES = {'xbox': (XBOX_TEXT, 'Xbox'), 'playstation': (PLAYSTATION_TEXT, 'PlayStation'), 'switch': (SWITCH_TEXT, 'Switch')}      # style -> (button names, brand)
FONT = 'shared-fonts/0-standard.bfttf'
BFTTF_MAGIC = 0x7f9a0218
SIZE = 48               # the key and face textures are SIZE x SIZE pixels, the d-pad of the shortcut icons DPAD_SIZE
DPAD_SIZE = 64
BAD_DATA = (ValueError, KeyError, IndexError, struct.error)      # what a file of another layout than expected raises
HINT = 'blyt/PaMessageTipsDrcImageNN_00.bflyt'          # the hint boxes: a ring and a dot on the button of a hint
HINT_PIECE = 'Nt_GamePadIllustSnn_02^f'                  # their picture of the face buttons
SPOT = {pad: spot for spot, pad in art.FACE_LAYOUT.items()}     # pad face button -> the Switch button's spot that our diamond draws it at
SWITCH_SPOT = {pad: pad for pad in 'abxy'}      # on the game's own Switch pictures a button sits at its own spot (the pad names its labels)


def bfttf_decode(blob):
    """A BFTTF (the Switch's system font files): big-endian words XOR-ed with word0 ^ 0x7f9a0218; word1 is the font's size."""
    key = struct.unpack_from('>I', blob, 0)[0] ^ BFTTF_MAGIC
    size = struct.unpack_from('>I', blob, 4)[0] ^ key
    body = blob[8:8 + size]
    words = struct.unpack(f'>{size // 4}I', body[:size - size % 4])
    return struct.pack(f'>{len(words)}I', *(w ^ key for w in words)) + body[size - size % 4:]


class Letters:
    """Outlines of the system font, as pixel contours."""
    def __init__(self, font):
        from fontTools.pens.recordingPen import RecordingPen
        self.font, self.pen = font, RecordingPen
        self.glyphs, self.cmap = font.getGlyphSet(), font.getBestCmap()
        self.em = font['head'].unitsPerEm

    def text(self, label, cap, cx, cy, max_width):
        """Contours of `label` with capital height `cap` pixels, centred on (cx, cy) (pixels, y down), at most max_width wide."""
        capital = self.pen(); self.glyphs[self.cmap[ord('H')]].draw(capital)
        ys = [p[1] for _, args in capital.value for p in args if p]
        scale = cap / (max(ys) - min(ys))
        x, parts = 0.0, []
        for ch in label:
            name = self.cmap[ord(ch)]
            rec = self.pen(); self.glyphs[name].draw(rec)
            parts.append((rec.value, x)); x += self.font['hmtx'][name][0]
        width = (x - 0) * scale
        if width > max_width:
            scale *= max_width / width; width = max_width
        contours = []
        for value, offset in parts:
            for contour in bf.flatten(value):
                contours.append([(cx - width / 2 + (px + offset) * scale, cy + cap / 2 - py * scale + (min(ys) * scale)) for px, py in contour])
        return contours


def png(rows, path, scale=4):
    raw = b''.join((b'\0' + b''.join(bytes([v]) * scale for v in r)) * scale for r in rows)
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', len(rows[0]) * scale, len(rows) * scale, 8, 0, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def style_parts(style):
    """(button names, brand) of a style; a style other than xbox, playstation and switch is refused."""
    if style not in STYLES:
        raise ValueError(f'style: {" or ".join(STYLES)}, not {style!r}')
    return STYLES[style]


LETTER_STROKE = 0.06    # how far the letters of the buttons grow outward on each side, in capital heights of the font


def _area(points):
    return sum(x * q[1] - q[0] * y for (x, y), q in zip(points, points[1:] + points[:1])) / 2


def grown_contour(points, d, miter=2.0):
    """The polygon `points` (counter-clockwise: its inside on the left) moved outward by `d` on each side; a hole (clockwise)
    shrinks, so the ink of a letter is always what grows. Outward corners are mitred up to `miter` times `d`, then cut off flat;
    inward corners (and every corner of a hole) take the plain mitre point. A hole that shrinks to nothing or turns over gives []
    (the counter fills)."""
    clean = [p for i, p in enumerate(points) if p != points[i - 1]]
    n, out, ends = len(clean), [], []       # ends: where the points of each corner stop in `out`
    for i, (x, y) in enumerate(clean):
        (ax, ay), (bx, by) = clean[i - 1], clean[(i + 1) % n]
        l1, l2 = math.hypot(x - ax, y - ay), math.hypot(bx - x, by - y)
        n1, n2 = ((y - ay) / l1, -(x - ax) / l1), ((by - y) / l2, -(bx - x) / l2)
        dot = n1[0] * n2[0] + n1[1] * n2[1]
        outward = (x - ax) * (by - y) - (y - ay) * (bx - x) > 0        # a turn away from the side that grows
        if 1 + dot > (2 / (miter * miter) if outward else 1e-6):
            k = d / (1 + dot)
            out.append((x + (n1[0] + n2[0]) * k, y + (n1[1] + n2[1]) * k))
        else:
            out += [(x + n1[0] * d, y + n1[1] * d), (x + n2[0] * d, y + n2[1] * d)]
        ends.append(len(out))
    before = _area(clean)
    if before < 0:      # a hole: if it has shrunk to nothing or turned over (most of its edges point the other way), the counter fills
        turned = 0
        for i in range(n):
            (px, py), (qx, qy) = out[ends[i] - 1], out[ends[i] % len(out)]      # the edge i of the offset
            (ax, ay), (bx, by) = clean[i], clean[(i + 1) % n]
            turned += (qx - px) * (bx - ax) + (qy - py) * (by - ay) <= 0
        if 2 * turned > n or _area(out) > 0.02 * before:
            return []
    return out


def drawers_of(style, letters):
    """pad control (and d-pad glyph code point, see DPAD_GLYPHS) -> (advance, draw) of the style's buttons: the drawings of
    make_button_icons.py, with their letters from `letters.font` (the Switch system font)."""
    style_parts(style)
    import make_button_icons as mbi
    from fontTools.pens.recordingPen import RecordingPen
    from fontTools.pens.reverseContourPen import ReverseContourPen

    class Glyphs(mbi.Glyphs):
        """make_button_icons.py cuts a letter out of a shape by drawing it the other way round, and takes the CFF fonts' outlines
        (outer contours counter-clockwise). The Switch system font is TrueType (outer contours clockwise): its outlines are turned.
        The game's own button letters are bolder than the font's regular weight (as high, but with thicker strokes): each outline
        is grown outward by LETTER_STROKE of the capital height, a length of the drawing, so the picture is the same at every scale."""
        def outline(self, ch):
            rec, bounds, upm, advance = super().outline(ch)
            if 'glyf' in self.fonts[0]:
                turned = RecordingPen(); rec.replay(ReverseContourPen(turned)); rec = turned
            font = self.fonts[0]
            cap = font['OS/2'].sCapHeight or 0.7 * upm
            grown = RecordingPen()
            ink = [1e9, 1e9, -1e9, -1e9]
            for contour in bf.flatten(rec.value):
                points = grown_contour(contour, LETTER_STROKE * cap)
                if not points:
                    continue
                grown.moveTo(points[0])
                for point in points[1:]:
                    grown.lineTo(point)
                grown.closePath()
                ink = [min(ink[0], *(p[0] for p in points)), min(ink[1], *(p[1] for p in points)),
                       max(ink[2], *(p[0] for p in points)), max(ink[3], *(p[1] for p in points))]
            return grown, tuple(ink) if grown.value else bounds, upm, advance + 2 * LETTER_STROKE * cap   # the growth keeps the gaps
    make = {'xbox': mbi.xbox_drawers, 'playstation': mbi.playstation_drawers, 'switch': switch_drawers}[style]
    g = Glyphs([letters.font])
    drawers, dpads = make(g)
    sticks = {pad: drawer for pad, drawer in switch_drawers(g)[0].items() if pad in STICKS}     # where the game has no picture
    return {**drawers, **sticks, **dpads}


def switch_drawers(g):
    """Drawers of the Switch's buttons by controller button (the keys of prompt_layout.prompt_controls, which on a Nintendo
    controller name its labels): A B X Y discs, L R ZL ZR keys, + and - on discs, the d-pad cross. The switch style traces the
    game's own pictures (traced); these drawings stand in only where a texture has no picture of the button pressed."""
    import make_button_icons as mbi

    def disc(label, advance=640):
        def draw(pen):
            mbi.circle(pen, advance / 2, mbi.CY, mbi.R); g.hole(pen, label, advance / 2, mbi.CY, mbi.R * 0.95, mbi.R * 1.3)
        return advance, draw

    def shoulder(label, advance):     # L R (about square) and ZL ZR (wider): keys as high as a disc, the letters large
        def draw(pen):
            mbi.rounded(pen, 40, mbi.Y0, advance - 40, mbi.Y1, 70); g.hole(pen, label, advance / 2, mbi.CY, 300, advance - 200)
        return advance, draw

    def sign(minus, advance=640):      # + and -: a bar (and a cross bar) cut out of a disc
        def draw(pen):
            cx, cy, a, w = advance / 2, mbi.CY, mbi.R * 0.50, mbi.R * 0.11
            mbi.circle(pen, cx, cy, mbi.R)
            points = ([(cx - a, cy - w), (cx + a, cy - w), (cx + a, cy + w), (cx - a, cy + w)] if minus else
                      [(cx - a, cy - w), (cx - w, cy - w), (cx - w, cy - a), (cx + w, cy - a), (cx + w, cy - w), (cx + a, cy - w),
                       (cx + a, cy + w), (cx + w, cy + w), (cx + w, cy + a), (cx - w, cy + a), (cx - w, cy + w), (cx - a, cy + w)])
            mbi.oriented(pen, points, hole=True)
        return advance, draw
    drawers = {'a': disc('A'), 'b': disc('B'), 'x': disc('X'), 'y': disc('Y'), 'l1': shoulder('L', 680), 'r1': shoulder('R', 680),
               'l2': shoulder('ZL', 840), 'r2': shoulder('ZR', 840), 'menu': sign(False), 'view': sign(True), 'l3': disc('L'), 'r3': disc('R')}
    return drawers, mbi.xbox_drawers(g)[1]


STICKS = ('l3', 'r3')      # the stick presses: every style shows the game's own picture of the stick (traced), as the switch style
SWITCH_TEXTURES = {**FACE, 'l1': SHAPE['l'][0], 'l2': SHAPE['zl'][0]}      # pad -> the texture of its Switch button
OWN_BUTTON = {**{name: pad for pad, name in SWITCH_TEXTURES.items()}, **dict.fromkeys(SHAPE['lstick'], 'l3'), 'ButtonIconL46NN_00^d': 0xE049}
DPAD_GLYPHS = (0xE049, 0xE060, 0xE061, 0xE062, 0xE063, 0xE064, 0xE065)      # the d-pad glyphs of the button font (0xE049: all four lit)
DPAD_TEXTURE = 'ButtonIconL46NN_00^d'       # the d-pad of the shortcut icons on the screen (four dots)


def levels(rows, share=0.02):
    """The flat grey levels of a picture: the peaks of its histogram (each within 6 of it holding at least `share` of the
    pixels), at least 25 apart. A pixel on an edge's ramp (between a darker and a brighter neighbour, across or down) is not
    counted, so a straight edge's anti-aliasing is not taken for a grey. A button picture has two levels (ground and white) or
    three (a grey ring or outline)."""
    h, w = len(rows), len(rows[0])
    count, total = [0] * 256, h * w
    for y, row in enumerate(rows):
        for x, v in enumerate(row):
            ramp = False
            for a, b in ((row[max(x - 1, 0)], row[min(x + 1, w - 1)]), (rows[max(y - 1, 0)][x], rows[min(y + 1, h - 1)][x])):
                ramp |= min(a, b) < v < max(a, b) and max(a, b) - min(a, b) > 100
            if not ramp:
                count[v] += 1
    near = [sum(count[max(0, v - 6):v + 7]) for v in range(256)]
    peaks = []
    for v in sorted(range(256), key=lambda v: (-near[v], -count[v])):      # the fullest windows first
        if near[v] < share * total:
            break
        top = max(range(max(0, v - 6), min(256, v + 7)), key=count.__getitem__)     # the window's most common value
        if all(abs(top - p) > 24 for p in peaks):
            peaks.append(top)
    return sorted(peaks) or [0]


def traced(rows, box, width, target, height=None, oversample=4):
    """The game's picture `rows` drawn again width x height (height: width when not given) and sharp: its part in `box` (x0, y0,
    x1, y1) made as large as fits in `target` (in the new picture's pixels, its shape kept, centred). Each sample reads the
    picture between its pixel centres (bilinear) and takes the nearest of its flat levels, so an edge lands where the game's
    anti-aliasing puts it, without the blur of an enlarged picture, and a grey part stays grey. The switch style draws every
    button so: it then looks as the game's own. The same picture (the fonts of every language share their button glyphs) is
    traced once."""
    return _traced(tuple(bytes(r) for r in rows), tuple(box), width, tuple(target), height or width, oversample)


@functools.lru_cache(maxsize=None)
def _traced(rows, box, width, target, height, oversample):
    flat = levels(rows)
    cuts = [(a + b) / 2 for a, b in zip(flat, flat[1:])]
    h, w = len(rows), len(rows[0])
    k = min((target[2] - target[0]) / (box[2] - box[0]), (target[3] - target[1]) / (box[3] - box[1]))
    ox, oy = (box[0] + box[2]) / 2 - (target[0] + target[2]) / 2 / k, (box[1] + box[3]) / 2 - (target[1] + target[3]) / 2 / k
    steps = [(i + 0.5) / oversample for i in range(oversample)]

    def taps(origin, count, size):      # per sample along an axis: (pixel, next pixel, weight of the next) between the centres
        out = []
        for p in range(count):
            row = []
            for t in steps:
                u = min(max(origin + (p + t) / k - 0.5, 0), size - 1)
                i = int(u); row.append((i, min(i + 1, size - 1), u - i))
            out.append(row)
        return out
    xs, ys = taps(ox, width, w), taps(oy, height, h)
    level = [flat[sum(v > c for c in cuts)] for v in range(256)]       # bilinear values rounded to 0..255, then to a flat level
    n = oversample * oversample
    out = []
    for column in ys:
        lines = [(rows[a], rows[b], f) for a, b, f in column]
        line = bytearray(width)
        for px, across in enumerate(xs):
            total = 0
            for top, bottom, fy in lines:
                for a, b, fx in across:
                    total += level[round((top[a] + (top[b] - top[a]) * fx) * (1 - fy) + (bottom[a] + (bottom[b] - bottom[a]) * fx) * fy)]
            line[px] = round(total / n)
        out.append(bytes(line))
    return out


def bright_box(rows, level=128):
    """(x0, y0, x1, y1) of the pixels of `rows` above `level` (x1 and y1 are one past the last pixel): the white shape of a key."""
    points = [(x, y) for y, row in enumerate(rows) for x, v in enumerate(row) if v > level]
    if not points:
        raise ValueError('no shape found in the picture')
    return min(p[0] for p in points), min(p[1] for p in points), max(p[0] for p in points) + 1, max(p[1] for p in points) + 1


def disc_of(rows, level=128):
    """(centre x, centre y, diameter) of the white disc of a face picture: the bounding box of its pixels above `level`."""
    x0, y0, x1, y1 = bright_box(rows, level)
    return (x0 + x1) / 2, (y0 + y1) / 2, ((x1 - x0) + (y1 - y0)) / 2


def disc_box(rows):
    """The square box of the white disc of `rows`: where a round button goes (a face button, Menu and View, the d-pad)."""
    cx, cy, d = disc_of(rows)
    return cx - d / 2, cy - d / 2, cx + d / 2, cy + d / 2


def stick_body(rows, bright=128):
    """The square box of the body of the stick in the stick press glyph `rows` (a font cell: the stick seen from the side, an arrow
    pressing down on its cap, a small L or R on it): as wide as the widest bright rows and on the place of those rows."""
    spans = {y: (min(x for x, v in enumerate(row) if v > bright), max(x for x, v in enumerate(row) if v > bright))
             for y, row in enumerate(rows) if any(v > bright for v in row)}
    if not spans:
        raise ValueError('no stick found in the glyph')
    widest = max(b - a + 1 for a, b in spans.values())
    body = [y for y, (a, b) in spans.items() if b - a + 1 >= 0.6 * widest]
    r = widest / 2
    cx = sum(spans[y][0] + spans[y][1] + 1 for y in body) / (2 * len(body))
    cy = (min(body) + max(body) + 1) / 2
    return cx - r, cy - r, cx + r, cy + r


def render(drawer, size, box, height=None):
    """The button drawn by `drawer` ((advance, draw) of make_button_icons.py) as a size x size picture (size x height when given:
    the 1.6.0 font cells are not square) (rows of bytes): the
    drawing's ink is made as large as fits in `box` (x0, y0, x1, y1 in the picture's pixels, y down) with its shape kept, and is
    centred in it. Every picture of a button, a texture or a font glyph, at any scale, is made by this one function."""
    from fontTools.pens.boundsPen import BoundsPen
    from fontTools.pens.recordingPen import RecordingPen
    pen = RecordingPen(); drawer[1](pen)
    bounds = BoundsPen(None); pen.replay(bounds)
    x0, y0, x1, y1 = bounds.bounds
    k = min((box[2] - box[0]) / (x1 - x0), (box[3] - box[1]) / (y1 - y0))
    cx, cy, mx, my = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2, (x0 + x1) / 2, (y0 + y1) / 2
    return bf.coverage_rect([[(cx + (x - mx) * k, cy - (y - my) * k) for x, y in c] for c in bf.flatten(pen.value, max(12, size // 4))],
                            size, height or size)


def enlarged(box, scale):
    return tuple(v * scale for v in box)


def key_rows(bntx, textures, name, size=SIZE):
    """The pixels (rows of bytes) of the R8 texture `name`, which must be a plain single-mip size x size one. A texture that is
    not there, or is in another format or size (another game version), raises ValueError."""
    texture = textures.get(name)
    if texture is None:
        raise ValueError('not found')
    if (texture.width, texture.height) != (size, size):
        raise ValueError(f'{texture.width}x{texture.height} pixels, not {size}x{size}')
    return bf.r8_read(bntx, texture)


def plan(bntx, textures, controls, letters, style='xbox', scale=1):
    """texture name -> new pixel rows (`scale` times the size of the game's texture) for the prompts `controls`
    (prompt_layout.prompt_controls) asks for, and notes. Each is `render` of the button that is pressed, fitted on the white shape
    of the game's picture (analysed at the game's size): the disc of the face buttons and the d-pad, the box of the keys L and ZL
    and of the stick press. A texture that is not there, or whose format, size or mip count is not the game's (another game
    version), or that holds no shape, gets a note and stays the game's."""
    text, brand = style_parts(style)
    drawers = drawers_of(style, letters)
    new, notes = {}, []

    def draw(name, pad, size, find):
        try:
            old = key_rows(bntx, textures, name, size)
            own = style == 'switch' or pad in STICKS
            source = (name if OWN_BUTTON.get(name) == pad else SWITCH_TEXTURES.get(pad)) if own else None
            if source:      # the switch style: the game's own picture of the button pressed, traced
                rows = old if source == name else key_rows(bntx, textures, source, size)
                new[name] = traced(rows, find(rows), len(old) * scale, enlarged(find(old), scale))
            else:
                new[name] = render(drawers[pad], len(old) * scale, enlarged(find(old), scale))
        except BAD_DATA as error:
            notes.append(f'{name}: {error}; the Switch icon stays')
    for slot, name in FACE.items():
        pad = controls[slot]
        if pad in FACE:
            draw(name, pad, SIZE, disc_box)
        else:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no face picture; the Switch icon stays')
    draw(DPAD_TEXTURE, 0xE049, DPAD_SIZE, disc_box)
    for slot, names in SHAPE.items():
        pad = controls[slot]
        if text.get(pad) is None:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no {brand} letter here; the Switch icon stays')
            continue
        for name in names:
            draw(name, pad, SIZE, bright_box)
    return new, notes


def plan_pictures(bntx, textures, controls, letters, style='xbox', scale=1):
    """texture name -> (grey rows, alpha rows) of the controller pictures of the controller tips page, drawn again as an Xbox
    controller or a DualSense (botw_controller_art.py), `scale` times as large as the game's, and notes. A button driven by a
    stick press carries no name (no LS, RS, L3 or R3: the sticks keep the game's look). A picture whose texture is not the size
    or format of the drawing (another game version) stays as it was, with a note."""
    text = {pad: name for pad, name in style_parts(style)[0].items() if pad not in STICKS}
    new, notes = {}, []
    for name, ((width, height), draw) in PICTURES.items():
        texture = textures.get(name)
        if texture is None or (texture.width, texture.height) != (width, height):
            notes.append(f'{name}: not found as a {width}x{height} texture; the Switch picture stays')
            continue
        try:
            bf.check_rg8(texture)
        except ValueError as error:
            notes.append(f'{error}; the Switch picture stays')
            continue
        new[name] = draw(controls, letters, text, style=style, **({'scale': scale} if scale != 1 else {}))
    return new, notes


def ring_edits(layout, inner, controls, spots=SPOT):
    """(offset, new bytes) edits of the layout archive `layout` (members `inner`) that move the ring and the dot of each hint box
    face button (panes N_A_00 to N_Y_00 of HINT) to the spot of the button the player presses for it (controls[slot]) on our
    drawn diamond (art.FACE_LAYOUT; `spots` SWITCH_SPOT: on the game's own diamond), and notes. The spots are the layout's own: where the Switch button's pane sits. A button
    that stays where it is, or that no face button drives, is not edited."""
    if HINT not in inner:
        return [], [f'{HINT}: not found; the hint box rings stay on the Switch buttons']
    lo, hi = inner[HINT]
    try:
        panes = bf.bflyt_panes(layout[lo:hi])
    except BAD_DATA as error:
        return [], [f'{HINT}: {error}; the hint box rings stay on the Switch buttons']
    groups = {}
    for offset, name, parent in panes:
        if parent and parent.startswith('N_') and parent[2:-3].lower() in FACE:
            groups.setdefault(parent[2:-3].lower(), {})[name.split('_')[1]] = offset      # slot -> {'Color': ring, 'Point': dot}
    if sorted(groups) != sorted(FACE) or any(sorted(g) != ['Color', 'Point'] for g in groups.values()):
        return [], [f'{HINT}: ring panes not found; the hint box rings stay on the Switch buttons']
    edits, notes = [], []
    for slot in FACE:
        pad = controls[slot]
        if pad not in spots:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no face button; its hint box ring stays')
        elif spots[pad] != slot:
            for part, offset in groups[slot].items():
                source = lo + groups[spots[pad]][part] + bf.PANE_TRANSLATION
                edits.append((lo + offset + bf.PANE_TRANSLATION, bytes(layout[source:source + 8])))
    return edits, notes


class MissingData(ValueError):
    """A file or archive member the tool cannot work without is not there."""


def make_pack(pack, controls, letters, style='xbox', pictures=True, scale=1):
    """Pack/Bootup.pack with the prompts of `controls` (prompt_layout.prompt_controls) redrawn, and with `pictures` the controller
    pictures of the controller tips page too (never for the switch style). Returns (the new pack, texture name -> new pixel rows, texture name -> (grey rows,
    alpha rows) of the pictures, notes, (Common.sblarc size before, after, decompressed size before, decompressed growth)).
    At scale 1 only the changed textures' bytes (and, with a remap and the redrawn face picture, the hint box ring positions:
    ring_edits) differ. At a larger scale the changed textures are new, `scale` times as large (botw_formats.bntx_resize), the layout
    archive grows by whole GROW_ALIGN steps, and every other member of it stays as it was. Every other member of the pack is the
    original. A texture that is missing or not as expected stays the game's, with a note (see plan, plan_pictures); an archive
    without Layout/Common.sblarc or its BNTX raises MissingData, a damaged one ValueError (and the like)."""
    members = bf.sarc_members(pack)
    if LAYOUT not in members:
        raise MissingData(f'{LAYOUT} not found in the archive')
    stream = pack[slice(*members[LAYOUT])]
    layout = bf.yaz0_decode(stream)
    inner = bf.sarc_members(layout)
    if BNTX not in inner:
        raise MissingData(f'{BNTX} not found in {LAYOUT}')
    start = inner[BNTX][0]
    bntx = layout[start:inner[BNTX][1]]
    textures = bf.bntx_textures(bntx)
    new, notes = plan(bntx, textures, controls, letters, style, scale)
    pictures = pictures and style != 'switch'      # the game's own pictures are the Switch's
    drawn, more = plan_pictures(bntx, textures, controls, letters, style, scale) if pictures else ({}, [])
    if scale == 1:
        edits = [(start + textures[name].offset, bf.r8_image(bntx, textures[name], rows)) for name, rows in sorted(new.items())]
        edits += [(start + textures[name].offset, bf.rg8_image(bntx, textures[name], *drawn[name])) for name in sorted(drawn)]
    else:
        for name in new:
            bf.check_r8(textures[name])
        images = {name: (len(rows[0]), len(rows), *bf.r8_pack(rows)) for name, rows in new.items()}
        for name, (grey, alpha) in drawn.items():      # (plan_pictures has checked their format)
            images[name] = (len(grey[0]), len(grey), *bf.rg8_pack(grey, alpha))
        edits = bf.sarc_member_edits(layout, BNTX, bf.bntx_resize(bntx, images)) if images else []
    if HINT_PIECE in drawn or style == 'switch':      # our diamond or the game's: the rings of the hint boxes go to the buttons pressed
        moved, more_notes = ring_edits(layout, inner, controls, SPOT if HINT_PIECE in drawn else SWITCH_SPOT)   # offsets as the archive was
        edits, more = edits + moved, more + more_notes
    spliced = bf.yaz0_splice(stream, layout, edits)
    new_pack = bf.sarc_replace(pack, LAYOUT, spliced)
    return new_pack, new, drawn, notes + more, (len(stream), len(spliced), len(layout), struct.unpack_from('>I', spliced, 4)[0] - len(layout))


FONT_FACE = {'a': 0xE040, 'b': 0xE041, 'x': 0xE042, 'y': 0xE043}              # Switch button -> code point in External_00.bffnt
FONT_SHAPE = {'l': 0xE044, 'r': 0xE045, 'zl': 0xE046, 'zr': 0xE047, 'lstick': 0xE056, 'rstick': 0xE057}
FONT_MENU = {'plus': 0xE04B, 'minus': 0xE04C}
BFFNT = 'External_00.bffnt'
SWITCH_GLYPHS = {**FONT_FACE, 'l1': 0xE044, 'r1': 0xE045, 'l2': 0xE046, 'r2': 0xE047, 'l3': 0xE056, 'r3': 0xE057, 'menu': 0xE04B, 'view': 0xE04C}
SWITCH_TRACED = (0xE040, 0xE068)        # the button glyphs of the font: above scale 1 all are traced (or drawn)


def font_cells(font, sheet_rows, controls, letters, style='xbox', scale=1):
    """code point -> new upright cell (`scale` times the size of the game's) for the prompts `controls` asks for, and notes (as
    plan() does for the textures): `render` of the button that is pressed, fitted on the place of the game's glyph (analysed at
    the game's size): the disc of A B X Y, + and - (the glyph sits in the left part of its cell and the game draws only that part)
    and the d-pad, the box of L R ZL ZR. A stick press is the game's own glyph of the stick pressed, traced on the body of the
    stick, in every style."""
    text, brand = style_parts(style)
    drawers = drawers_of(style, letters)
    size, height, new, notes = font.cell * scale, font.cell_height * scale, {}, []

    def draw(code, pad, find, what):
        try:
            old = font.get(sheet_rows, code)
            if style == 'switch' or pad in STICKS:       # the game's own glyph of the button pressed, traced
                rows = font.get(sheet_rows, SWITCH_GLYPHS.get(pad, code))
                new[code] = traced(rows, find(rows), size, enlarged(find(old), scale), height)
            else:
                new[code] = render(drawers[pad], size, enlarged(find(old), scale), height)
        except ValueError as error:     # no shape found: the glyph stays as it was
            notes.append(f'{what}: {error}; the Switch glyph stays')
    for slot, code in FONT_FACE.items():
        pad = controls[slot]
        if pad not in FONT_FACE:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no face glyph; the Switch glyph stays')
        elif code in font.cells:
            draw(code, pad, disc_box, f'{slot.upper()} glyph')
    for slot, code in FONT_SHAPE.items():
        pad = controls[slot]
        if code not in font.cells:
            continue
        if text.get(pad) is None:
            notes.append(f'{slot.upper()}: {pad or "nothing"} has no {brand} letter here; the Switch glyph stays')
            continue
        draw(code, pad, stick_body if slot in ('lstick', 'rstick') else bright_box, f'{slot.upper()} glyph')
    for code in DPAD_GLYPHS:     # the d-pad: a cross on the place of the game's four dots
        if code in font.cells:
            draw(code, code, disc_box, f'd-pad glyph {code:X}')
    for slot, code in FONT_MENU.items():
        kind = controls[slot]
        if kind in ('menu', 'view') and code in font.cells:
            draw(code, kind, disc_box, f'{slot} glyph')
        else:
            notes.append(f'{slot}: {kind or "nothing"} has no {brand} picture here; the Switch glyph stays')
    if scale > 1:     # every other button glyph (a stick being moved, a button nothing drives) traced too, in every style
        for code in sorted(font.cells):
            if code not in new and SWITCH_TRACED[0] <= code < SWITCH_TRACED[1]:
                old = font.get(sheet_rows, code)
                if max(map(max, old)) > 128:
                    box = (0, 0, len(old[0]), len(old))
                    new[code] = traced(old, box, size, enlarged(box, scale), height)
    return new, notes


def make_font(stream, controls, letters, style='xbox', scale=1):
    """A Font/Font_<language>.sbfarc (Yaz0 of a SARC) with the glyphs of `controls` redrawn in its External_00.bffnt. Returns
    (the new file, glyph code point -> new cell, notes, growth of the decompressed archive in bytes). At scale 1 only the changed
    blocks of the glyph sheet differ. At a larger scale the whole sheet is `scale` times as wide and high (botw_formats.
    Bffnt.grow_sheet): the glyphs that stay are enlarged by repeating their pixels (nearest), the redrawn ones are drawn at the
    larger size. A file with no External_00.bffnt, or whose glyph sheet is not as expected (format, more than one sheet, kind of
    CMAP, grid), raises (MissingData, ValueError and the like): the caller leaves the game's font file alone."""
    archive = bf.yaz0_decode(stream)
    members = bf.sarc_members(archive)
    if BFFNT not in members:
        raise MissingData(f'no {BFFNT} in the archive')
    start, end = members[BFFNT]
    data = archive[start:end]
    font = bf.Bffnt(data)
    if (font.texture.width % 4 or font.texture.height % 4 or (font.cell + 1) * font.columns > font.texture.width
            or (font.cell_height + 1) * font.rows > font.texture.height or max(font.cells.values(), default=0) >= font.columns * font.rows):
        raise ValueError(f'the glyph grid ({font.columns}x{font.rows} cells of {font.cell}x{font.cell_height}) does not fit the '
                         f'{font.texture.width}x{font.texture.height} sheet')
    sheet = data[font.sheet_at:]
    rows = bf.bc4_read(sheet, font.texture)
    new, notes = font_cells(font, rows, controls, letters, style, scale)
    if scale > 1:
        big = []
        for row in rows:
            wide = bytearray(len(row) * scale)
            for k in range(scale):
                wide[k::scale] = row
            big += [bytearray(wide) for _ in range(scale)]
        for code, cell in sorted(new.items()):
            x0, y0, x1, y1 = enlarged(font.cell_box(code), scale)
            for y, line in zip(range(y0, y1), reversed(cell)):
                big[y][x0:x1] = line
        edits = bf.sarc_member_edits(archive, BFFNT, font.grow_sheet(big))
        return bf.yaz0_splice(stream, archive, edits), new, notes, sum(len(b) - old for _, old, b in bf.full_edits(edits))
    boxes = [font.put(rows, code, cell) for code, cell in sorted(new.items())]
    image = bf.bc4_image(sheet, font.texture, rows, boxes)
    base = start + font.sheet_at + font.texture.offset
    old = archive[base:base + len(image)]
    edits, i = [], 0
    while i < len(image):       # the runs of bytes that changed
        if image[i] == old[i]:
            i += 1
            continue
        j = i
        while j < len(image) and (image[j] != old[j] or (j + 1 < len(image) and image[j + 1] != old[j + 1])):
            j += 1
        edits.append((base + i, image[i:j])); i = j
    return bf.yaz0_splice(stream, archive, edits), new, notes, 0


def resource_name(name):
    """The resource name of a RomFS file whose extension starts with the Nintendo compression letter s: Layout/Common.sblarc ->
    Layout/Common.blarc (the key of the resource size table)."""
    stem, _, ext = name.rpartition('.')
    return f'{stem}.{ext[1:]}' if ext.startswith('s') else name


def mod_table(mods):
    """The resource size table that the mods in folder `mods` replace, as (Yaz0 bytes, SHA-256), or None when their manifest.tsv
    does not list it (or there is none). A file that does not match its manifest line is refused (the game would stop on it)."""
    try:
        lines = (mods / 'manifest.tsv').read_text().splitlines()
    except FileNotFoundError:
        return None
    for line in lines:
        fields = line.split('\t')
        if fields[0] == bf.RSTB and len(fields) == 4:
            data = (mods / bf.RSTB).read_bytes()
            if hashlib.sha256(data).hexdigest() != fields[2]:
                raise ValueError(f'{mods / bf.RSTB} does not match its line in {mods / "manifest.tsv"}')
            return data, fields[2]
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder to write into (other files in it are kept and listed too)')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path, help='the built Breath of the Wild.app')
    source.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and runtime-index.bin')
    parser.add_argument('--version', choices=[v for v in APP_VERSIONS if v.startswith('botw')], help='the game version (default: the mod folder name)')
    parser.add_argument('--style', choices=list(STYLES), default='xbox', help='the buttons to show: xbox (default), playstation or switch (the game\'s own names, sharp at --scale 2 and up)')
    parser.add_argument('--face-buttons', choices=FACE_MODES, default='position',
                        help='how the controller\'s face buttons map (the runtime\'s TOTK_FACE_BUTTONS, or key 6): position (Xbox, PlayStation; default) or label (Switch)')
    parser.add_argument('--controls', type=Path, help='your controls.txt (the prompts then show the button you press)')
    parser.add_argument('--replace', action='store_true', help=f'overwrite a {PACK}, Font/Font_*.sbfarc or (with --scale above 1) {bf.RSTB} already in the mod folder')
    parser.add_argument('--scale', type=int, choices=range(1, 5), default=1,
                        help='draw every picture this many times as large (1 to 4; default 1, the game\'s size): sharper on a big screen, and the archives and the resource size table grow')
    parser.add_argument('--keep-switch-pictures', action='store_true',
                        help='leave the controller pictures of the controller tips page as the Switch\'s (otherwise an Xbox controller or a DualSense is drawn there)')
    parser.add_argument('--mods', type=Path, help='the player\'s mod folder: with --scale above 1, a resource size table its manifest.tsv replaces is raised instead of the game\'s')
    parser.add_argument('--preview', type=Path, help='also write a picture of the changed textures (PNG)')
    args = parser.parse_args()
    version = args.version or (args.mod.resolve().name if args.mod.resolve().name in APP_VERSIONS else None)
    romfs, app_fonts = args.romfs, None
    if args.app:
        if not version:
            parser.error('name the game version with --version (or name the mod folder botw160 or botw190)')
        romfs = args.app / f'Contents/Resources/totk/extracted/{version}/romfs'
        app_fonts = args.app / 'Contents/Resources/totk/extracted/firmware'
    if app_fonts is None:
        app_fonts = romfs.parent.parent / 'firmware'
    for need in ('romfs.bin', 'runtime-index.bin'):
        if not (romfs / need).is_file():
            parser.error(f'{romfs / need} not found')
    if not (app_fonts / FONT).is_file():
        parser.error(f'{app_fonts / FONT} not found (the app\'s copy of the Switch system fonts)')
    index = read_index(romfs / 'runtime-index.bin')
    if PACK not in index:
        parser.error(f'{PACK} not found in the RomFS ({romfs / "runtime-index.bin"}); nothing written')
    fonts = sorted(n for n in index if n.startswith('Font/Font_') and n.endswith('.sbfarc'))
    rstb, mods_rstb, scale = None, None, args.scale
    if scale > 1:      # the resource size table is read first: archives that grow need their entries raised, else nothing grows
        try:
            if bf.RSTB not in index:
                raise ValueError('not found in the RomFS')
            with (romfs / 'romfs.bin').open('rb') as image:
                offset, size = index[bf.RSTB]; image.seek(offset); rstb = image.read(size)
            mods_rstb = mod_table(args.mods) if args.mods else None
            for table in (rstb, *(mods_rstb[:1] if mods_rstb else ())):
                bf.rstb_edits(bf.yaz0_decode(table), dict.fromkeys((resource_name(LAYOUT), *map(resource_name, fonts)), 0))
        except (OSError, *BAD_DATA) as error:
            print(f'note: {bf.RSTB}: {error}; drawing at --scale 1 instead (nothing grows, so the table is not needed)')
            rstb, mods_rstb, scale = None, None, 1
    exists = [n for n in (PACK, *fonts, *([bf.RSTB] if scale > 1 else [])) if (args.mod / n).exists()]
    if exists and not args.replace:      # checked before anything is written
        parser.error(f'{args.mod / exists[0]} already exists (another mod\'s, or an earlier run): pass --replace to overwrite it')
    font_notes = []
    try:
        controls = prompt_controls(args.controls.read_text() if args.controls else '', args.face_buttons)
    except LayoutError as error:
        parser.error(f'{args.controls}, {error}')
    except (OSError, UnicodeDecodeError) as error:
        parser.error(f'{args.controls}: {error}')
    print('\n'.join(describe(args.controls.read_text() if args.controls else '', args.face_buttons, args.style, (*FACE, *SHAPE))))

    from fontTools.ttLib import TTFont
    letters = Letters(TTFont(io.BytesIO(bfttf_decode((app_fonts / FONT).read_bytes()))))
    with (romfs / 'romfs.bin').open('rb') as image:
        offset, size = index[PACK]; image.seek(offset); pack = image.read(size)
    while True:
        try:
            new_pack, new, drawn, notes, layout_sizes = make_pack(pack, controls, letters, args.style, not args.keep_switch_pictures, scale)
            break
        except MissingData as error:
            parser.error(f'{PACK}: {error}; nothing written')
        except BAD_DATA as error:
            if scale == 1:
                parser.error(f'{PACK}: {error}; nothing written')
            print(f'note: {PACK}: {error}; drawing at --scale 1 instead')
            scale, rstb = 1, None
    for note in notes:
        print('note:', note)
    target = args.mod / PACK
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(new_pack)
    print(f'{target}: {len(new) + len(drawn)} textures changed ({len(drawn)} controller pictures); {PACK} {len(pack)} -> {len(new_pack)} bytes, {LAYOUT} {layout_sizes[0]} -> '
          f'{layout_sizes[1]} (decompressed {layout_sizes[2]} bytes, {"unchanged" if not layout_sizes[3] else f"+{layout_sizes[3]}"})')
    growth = {resource_name(LAYOUT): layout_sizes[3]} if layout_sizes[3] else {}
    if args.preview and new:
        names = sorted(new)
        height = max(len(new[name]) for name in names)     # the keys are 48 pixels, the shortcut d-pad 64
        rows = [b''] * height
        for name in names:
            pic = [row for row in new[name]] + [bytes([128]) * len(new[name][0])] * (height - len(new[name]))
            rows = [a + b + bytes([128]) for a, b in zip(rows, pic)]
        png(rows, args.preview)
        print(f'{args.preview}: {", ".join(n.split("^")[0].replace("Nt_KeyTex", "") for n in names)}')
    if args.preview and drawn:
        width, rows = max(len(grey[0]) for grey, _ in drawn.values()), []
        for name in sorted(drawn, key=lambda n: (-len(drawn[n][0][0]), n)):       # the pictures over a mid grey, one under the other
            rows += [bytes((g * a + 90 * (255 - a)) // 255 for g, a in zip(grey, alpha)).ljust(width, bytes([128])) for grey, alpha in zip(*drawn[name])]
            rows.append(bytes([128]) * width)
        target = args.preview.with_name(args.preview.stem + '-pictures.png')
        png(rows, target, 2)
        print(f'{target}: {", ".join(sorted(drawn))}')
    for name in fonts:
        offset, size = index[name]
        with (romfs / 'romfs.bin').open('rb') as image:
            image.seek(offset); stream = image.read(size)
        try:
            data, cells, font_notes, grown = make_font(stream, controls, letters, args.style, scale)
        except BAD_DATA as error:
            print(f'note: {name}: {error}; skipped, the game keeps its own font file')
            continue
        if grown:
            growth[resource_name(name)] = grown
        (args.mod / name).parent.mkdir(parents=True, exist_ok=True)
        (args.mod / name).write_bytes(data)
        print(f'{args.mod / name}: {len(cells)} glyphs changed; {size} -> {len(data)} bytes' + (f' (decompressed +{grown})' if grown else ''))
    for note in dict.fromkeys(font_notes) if fonts else ():
        print('note (font glyphs):', note)
    if growth:      # the game sets memory aside for each resource by this table: it must hold what the archives grew
        if mods_rstb:   # the mods' table: each entry to the larger of the mod's size and the game's size plus the growth
            game, mine = bf.rstb_sizes(bf.yaz0_decode(rstb), growth), bf.rstb_sizes(bf.yaz0_decode(mods_rstb[0]), growth)
            growth = {name: max(0, game[name] + grow - mine[name]) for name, grow in growth.items()}
            rstb = mods_rstb[0]
        table = bf.yaz0_decode(rstb)
        data = bf.yaz0_splice(rstb, table, bf.rstb_edits(table, growth))
        (args.mod / bf.RSTB).parent.mkdir(parents=True, exist_ok=True)
        (args.mod / bf.RSTB).write_bytes(data)
        print(f'{args.mod / bf.RSTB}: {"from the mods, " if mods_rstb else ""}raised {", ".join(f"{name} by {grow}" for name, grow in sorted(growth.items()))}')
    # --mods naming the folder written into: its own table is raised in place, so the original stays the game's
    in_place = args.mods and args.mods.resolve() == args.mod.resolve()
    found = write_manifest(args.mod, romfs, merged={bf.RSTB: mods_rstb[1]} if growth and mods_rstb and not in_place else None)
    if found.misspelt:
        sys.exit('manifest not written: ' + ', '.join(f'{a} (the RomFS has {b})' for a, b in found.misspelt))
    print(f'{args.mod / "manifest.tsv"}: {len(found.lines)} files ({len(found.lines) - len(found.added)} replaced, {len(found.added)} added)')


if __name__ == '__main__':
    main()
