#!/usr/bin/env python3
"""Draw the game's UI layouts as labelled rectangles, to check the layout edits for a canvas shape without running the game
(scripts/tall_layout.py, scripts/ultrawide_layout.py).

Reads UI/LayoutArchive/Common.Product.*.blarc from the IVFC-verified RomFS image (extracted/<version>/romfs). For every layout
it draws three panels: the 16:9 original, the original stretched over a canvas of the given shape (what the game draws without
edits), and the edited layouts on that canvas. Panes are rectangles from their position, scale, size and origin; parts (prt1)
are expanded with the layout they name (its children hang from the part pane; its RootPane is not drawn). No textures, text or animations: this shows geometry only (a pane the layout hides is
skipped; panes an animation shows or moves are drawn where the layout puts them).

usage: layout_view.py <width>x<height> [--layout NAME ...] [--out DIR] [--version v143] [--hud corner|center]
Without --layout every layout that is not a part (Pa + a capital letter) is drawn. A summary line per layout says how many full-screen panes
stop short of the canvas and how many panes leave it.
"""
import argparse, importlib.util, json, struct
from pathlib import Path
import compression.zstd as zstd
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
PANES = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
COLORS = {b'pic1': (230, 160, 60), b'txt1': (80, 200, 120), b'wnd1': (200, 100, 200), b'bnd1': (150, 150, 150),
          b'prt1': (90, 160, 255), b'pan1': (120, 120, 120), b'ali1': (120, 120, 120), b'scr1': (220, 90, 90)}

def sarc_members(data):
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]; names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        offset = names + (attribute & 0xffffff) * 4
        out[data[offset:data.index(0, offset, start)].decode()] = (start + lo, start + hi)
    return out

def load_blarc(version):
    manifest = json.loads((root / f'extracted/{version}/romfs/index.json').read_text())
    files = {f['path']: f for f in manifest['files']}
    with (root / f'extracted/{version}/romfs/romfs.bin').open('rb') as image:
        def read(path): f = files[path]; image.seek(f['offset']); return image.read(f['size'])
        dictionaries = {}
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        for lo, hi in sarc_members(pack).values():
            d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
        path = next(p for p in files if p.startswith('UI/LayoutArchive/Common.Product.') and p.endswith('.blarc.zs'))
        raw = read(path)
    blarc = bytearray(zstd.decompress(raw, zstd_dict=dictionaries[zstd.get_frame_info(raw).dictionary_id]))
    return blarc, sarc_members(blarc)

class Pane:
    def __init__(self, magic, name, visible, origin, t, s, size, part=None):
        self.magic, self.name, self.visible, self.origin, self.t, self.s, self.size, self.part = magic, name, visible, origin, t, s, size, part
        self.children = []

def parse(blarc, members, layout):
    """The pane tree of a layout (its RootPane) and the layout's screen size."""
    lo, hi = members[f'blyt/{layout}.bflyt']
    head, count = struct.unpack_from('<H', blarc, lo + 6)[0], struct.unpack_from('<H', blarc, lo + 0x10)[0]
    pos, stack, last, top, screen = lo + head, [], None, None, (1920.0, 1080.0)
    for _ in range(count):
        magic, size = bytes(blarc[pos:pos + 4]), struct.unpack_from('<I', blarc, pos + 4)[0]
        if magic == b'lyt1': screen = struct.unpack_from('<ff', blarc, pos + 12)
        elif magic in PANES:
            flag, origin = blarc[pos + 8], blarc[pos + 9]
            name = bytes(blarc[pos + 12:pos + 36]).split(b'\0')[0].decode()
            part = None
            if magic == b'prt1':
                n = struct.unpack_from('<I', blarc, pos + 84)[0]
                at = pos + 84 + 12 + n * 0x28
                part = bytes(blarc[at:blarc.index(0, at)]).decode()
            p = Pane(magic, name, bool(flag & 1), origin, struct.unpack_from('<ff', blarc, pos + 44),
                     struct.unpack_from('<ff', blarc, pos + 68), struct.unpack_from('<ff', blarc, pos + 76), part)
            if stack: stack[-1].children.append(p)
            if top is None: top = p
            last = p
        elif magic == b'pas1': stack.append(last)
        elif magic == b'pae1': stack.pop()
        pos += size
    return top, screen

AX = {0: 0, 1: -1, 2: 1}   # origin nibble -> where on the rectangle (x: left -1, right +1)
AY = {0: 0, 1: 1, 2: -1}   # (y: top +1, bottom -1; the layout's y points up)

def place(pane, parent, out, blarc, members, depth=0):
    """Append (pane, cx, cy, w, h) in layout units (y up) for the pane and everything under it."""
    pcx, pcy, pw, ph, psx, psy = parent
    ox, oy, px, py = AX[pane.origin & 3], AY[(pane.origin >> 2) & 3], AX[(pane.origin >> 4) & 3], AY[(pane.origin >> 6) & 3]
    sx, sy = psx * pane.s[0], psy * pane.s[1]
    w, h = pane.size[0] * sx, pane.size[1] * sy
    ax, ay = pcx + px * pw / 2 + pane.t[0] * psx, pcy + py * ph / 2 + pane.t[1] * psy
    cx, cy = ax - ox * w / 2, ay - oy * h / 2
    out.append((pane, cx, cy, w, h))
    frame = (cx, cy, w, h, sx, sy)
    for c in pane.children: place(c, frame, out, blarc, members, depth)
    if pane.magic == b'prt1' and pane.part and depth < 6 and f'blyt/{pane.part}.bflyt' in members:
        top, _ = parse(blarc, members, pane.part)
        for c in (top.children if top else []): place(c, frame, out, blarc, members, depth + 1)   # a part's RootPane is not instantiated

def flatten(blarc, members, layout):
    top, screen = parse(blarc, members, layout)
    out = []
    if top: place(top, (0, 0, screen[0], screen[1], 1.0, 1.0), out, blarc, members)
    return [r for r in out if r[0].visible], screen

def draw(rects, width_px, height_px, ystretch, title):
    """A panel: layout units -> pixels at half size, the canvas width_px x height_px (before the half), y stretched."""
    k = 0.62
    img = Image.new('RGB', (int(width_px * k), int(height_px * k) + 16), (24, 24, 28)); d = ImageDraw.Draw(img)
    d.text((4, 2), title, fill=(255, 255, 255))
    ox, oy = 0, 16
    def box(cx, cy, w, h):
        x0, x1 = (width_px / 2 + cx - w / 2) * k, (width_px / 2 + cx + w / 2) * k
        y0, y1 = (height_px / 2 - (cy + h / 2) * ystretch) * k + oy, (height_px / 2 - (cy - h / 2) * ystretch) * k + oy
        return min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)   # a negative scale flips a pane
    for pane, cx, cy, w, h in rects:
        x0, y0, x1, y1 = box(cx, cy, w, h)
        if abs(x1 - x0) > 4000 or abs(y1 - y0) > 4000: continue   # masks and other panes far larger than the screen
        c = COLORS[pane.magic]
        if pane.magic == b'pic1' and x1 - x0 > width_px * k * 0.9 and y1 - y0 > height_px * k * 0.5: d.rectangle((x0, y0, x1, y1), outline=(255, 255, 90), width=2)   # a backdrop
        elif pane.magic == b'pic1': d.rectangle((x0, y0, x1, y1), fill=tuple(v // 4 for v in c), outline=c)
        else: d.rectangle((x0, y0, x1, y1), outline=c)
        if pane.magic in (b'txt1', b'prt1', b'wnd1') and 20 < x1 - x0 < 900: d.text((x0 + 2, y0 + 1), pane.name, fill=c)
    d.rectangle((0, oy, int(width_px * k) - 1, int(height_px * k) + oy - 1), outline=(255, 255, 255))
    return img

def check(orig, edited, screen, canvas_h):
    """(full-screen panes of the original that stop short of the canvas in the edited layout, panes that leave the canvas); the
    canvas is the 1080-unit-high grid in both, whatever its shape (the game maps the grid onto it)."""
    full = {r[0].name for r in orig if r[0].magic in (b'pic1', b'pan1') and r[3] >= screen[0] * 0.98 and r[4] >= screen[1] * 0.98 and abs(r[2]) < 40}
    short = [r[0].name for r in edited if r[0].name in full and r[0].magic == b'pic1' and r[4] < 1080 * 0.98 and r[3] > 1000]
    gone = [r[0].name for r in edited if abs(r[2]) - r[4] / 2 > 540 + 60 and r[3] < 1900 and r[4] < 600 and r[0].magic in (b'pic1', b'txt1', b'prt1')]
    return sorted(set(short)), sorted(set(gone))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('size'); parser.add_argument('--layout', nargs='*'); parser.add_argument('--out', type=Path, default=Path('layout-view'))
    parser.add_argument('--stretched', action='store_true', help='also draw the original stretched over the canvas'); parser.add_argument('--version', default='v143'); parser.add_argument('--hud', choices=('corner', 'center'), default='corner')
    args = parser.parse_args()
    width, height = (int(v) for v in args.size.lower().split('x'))
    script = 'ultrawide_layout.py' if width * 9 > height * 16 else 'tall_layout.py'
    spec = importlib.util.spec_from_file_location(script[:-3], Path(__file__).resolve().parent / script)
    edit = importlib.util.module_from_spec(spec); spec.loader.exec_module(edit)
    blarc, members = load_blarc(args.version)
    edited = bytearray(blarc); edit.apply(edited, members, width, height, hud_corner=args.hud == 'corner')
    canvas_w, canvas_h = 1920, round(1920 * height / width)
    stretch = canvas_h / 1080
    args.out.mkdir(parents=True, exist_ok=True)
    names = args.layout or sorted(m[5:-6] for m in members if m.startswith('blyt/') and m.endswith('.bflyt') and not (m[5:7] == 'Pa' and m[7:8].isupper()))
    for name in names:
        before, screen = flatten(blarc, members, name); after, _ = flatten(edited, members, name)
        panels = [draw(before, 1920, 1080, 1.0, f'{name}: 16:9 original'), draw(after, canvas_w, canvas_h, stretch, f'edited, on {args.size}')]
        if args.stretched: panels.insert(1, draw(before, canvas_w, canvas_h, stretch, f'as is on {args.size}'))
        sheet = Image.new('RGB', (sum(p.width for p in panels) + 8, max(p.height for p in panels)), (0, 0, 0)); x = 0
        for p in panels: sheet.paste(p, (x, 0)); x += p.width + 4
        sheet.save(args.out / f'{name}.png')
        short, gone = check(before, after, screen, canvas_h)
        if short or gone: print(f'{name}: full-screen short {short[:4]}  off-canvas {gone[:4]}')
    print(f'{len(names)} layouts in {args.out}')
