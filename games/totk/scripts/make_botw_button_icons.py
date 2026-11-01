#!/usr/bin/env python3
"""Make a RomFS mod that shows, in Breath of the Wild's prompts, the button that drives each Switch input in the player's layout.

Breath of the Wild 1.9.0 draws its button icons from Font/Font_<region>.sbfarc -> External_00.bffnt, a bitmap font
whose one sheet (256x1024, BC4, 5 x 20 cells of 50x50 on a 51-pixel pitch, stored upside down) holds 50 icons for
the private-use code points U+E040-E091 (CMAP method 1: a table of glyph indices, one per code point). The seven region
archives (CN EU JP KR TH TW US) carry the same External_00.bffnt. Unlike Tears of the Kingdom (make_button_icons.py
redraws its outline font), nothing is drawn here: the layout only moves inputs between the buttons the font already
shows, so the code point of each input is pointed at the icon of the button that drives it (the A prompt shows B when
the right face button drives A). The sheet, the glyph widths and every other byte stay as they are, so each archive
keeps its size and the resource size table needs no change. Each archive is written back as Yaz0 with stored blocks only
(no back references; the game decompresses either), keeping the header's alignment word (0x1000 in 1.9.0). An archive
already in the mod folder is replaced (or removed, when the layout leaves its icons as they are) only when it is this
script's own, the game's archive with nothing but that table changed: another mod's font archive stops the run.

Which icon each input gets comes from the builder's model (tools/builder/icons_plan.botw_glyphs, from the layout of
controls.txt as tools/builder/controls_file reads it): the face letters of an Xbox controller by position, LB RB LT RT
as L R ZL ZR, Menu and View as + and -, the stick presses and the d-pad arms as their own icons, a cross for an input no
button drives. A combined d-pad icon whose inputs are not all on the d-pad keeps its icon (nothing to draw it with).
PlayStation symbols are not in the font, so a PlayStation controller gets the Xbox letters; a Nintendo controller (by
label, controller-type.txt 'switch') is refused, as the builder's icons for Tears of the Kingdom refuse it.

Checked on 1.9.0 (the code points of 1.6.0 are not checked). Needs no package beyond the standard library.
usage: make_botw_button_icons.py <mod folder> (--app <Breath of the Wild.app> | --romfs <folder>) [--version botw190]
                                 [--controls <controls.txt> | --glyphs <json>] [--write-all]
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))   # the builder runs it with -I, which leaves the folder out
import make_mod_manifest  # noqa: E402
from make_mod_manifest import app_image_dir, image_file, manifest, read_index, write_manifest  # noqa: E402
from prepare_shader_seed import yaz0  # noqa: E402

REPO = Path(__file__).resolve().parents[3]
ARCHIVES = 'Font/Font_'                             # Font/Font_<region>.sbfarc
MEMBER = 'External_00.bffnt'
DATA = Path.home()/'Library/Application Support/MacNativeGames/BotW'


def sarc_member(sarc, name):
    """(start, end) of a member of a little-endian SARC."""
    assert sarc[:4] == b'SARC' and sarc[6:8] == b'\xff\xfe', 'not a little-endian SARC'
    data = struct.unpack_from('<I', sarc, 0x0c)[0]; nodes = struct.unpack_from('<H', sarc, 0x1a)[0]
    names = 0x20 + nodes*16 + 8
    for k in range(nodes):
        _, attr, a, b = struct.unpack_from('<IIII', sarc, 0x20 + k*16)
        at = names + (attr & 0xffffff)*4
        if sarc[at:sarc.index(b'\0', at)].decode() == name:
            return data + a, data + b
    raise KeyError(name)


def cmap_table(ffnt):
    """(offset of the glyph index table, first code point, last code point) of the font's one table CMAP."""
    assert ffnt[:4] == b'FFNT', 'not a BFFNT'
    k, found = struct.unpack_from('<H', ffnt, 6)[0], []
    while k + 8 <= len(ffnt):
        magic, size = ffnt[k:k+4], struct.unpack_from('<I', ffnt, k + 4)[0]
        if magic == b'CMAP':
            lo, hi, method = struct.unpack_from('<IIH', ffnt, k + 8)
            assert method == 1, f'CMAP method {method}, expected 1 (a table)'
            found.append((k + 24, lo, hi))
        if size < 8:
            break
        k += size
    assert len(found) == 1, f'{len(found)} CMAP blocks, expected one'
    return found[0]


def remap(ffnt, glyphs):
    """ffnt with each code point of glyphs (code point -> code point) showing the other's icon (a new copy)."""
    table, lo, hi = cmap_table(ffnt)
    index = lambda cp: struct.unpack_from('<H', ffnt, table + (cp - lo)*2)[0]  # noqa: E731
    out = bytearray(ffnt)
    for cp, shown in glyphs.items():
        assert lo <= cp <= hi and lo <= shown <= hi, f'U+{cp:X} or U+{shown:X} outside U+{lo:X}-U+{hi:X}'
        struct.pack_into('<H', out, table + (cp - lo)*2, index(shown))
    return bytes(out)


def sarc_put(sarc, name, body):
    """sarc with member name holding body: the members after it move by a multiple of their largest alignment."""
    data = struct.unpack_from('<I', sarc, 0x0c)[0]; nodes = struct.unpack_from('<H', sarc, 0x1a)[0]
    a, b = sarc_member(sarc, name); grow = len(body) - (b - a)
    after = [k for k in range(nodes) if data + struct.unpack_from('<I', sarc, 0x20 + k*16 + 8)[0] >= b]
    step = max([min((data + struct.unpack_from('<I', sarc, 0x20 + k*16 + 8)[0]) & -(data + struct.unpack_from(
        '<I', sarc, 0x20 + k*16 + 8)[0]), 0x2000) for k in after] or [1])
    shift = -(-grow // step)*step
    out = bytearray(sarc[:a]) + body + bytes(shift - grow) + sarc[b:]
    for k in range(nodes):
        at = 0x20 + k*16 + 8; start, end = struct.unpack_from('<II', out, at)
        if data + start == a:
            struct.pack_into('<I', out, at + 4, end + grow)
        elif k in after:
            struct.pack_into('<II', out, at, start + shift, end + shift)
    struct.pack_into('<I', out, 8, len(out))
    return bytes(out)


# ---- the action guide (Pack/Bootup.pack -> Layout/Common.sblarc -> blyt/PaDoCommand<A|B|X|Y>_00.bflyt) ----
# Each of the four parts draws the four face buttons as a diamond around N_Loop_00: three letters (text panes) and its own
# button (N_Btn*: the key texture Nt_KeyTex<letter>_00^d, the window and the action's name beside it), in the Switch's
# place (A right, B bottom, X top, Y left), plus copies of the name (N_Capture*, N_Attention_00) placed from it. The
# player's diamond is the Xbox one (A bottom, B right, X left, Y top) with the button that drives the input lit. A button
# moved to or from the left takes Y's mirror image (the name on the left): x negated, the window's scale -1, the names
# right-aligned, as PaDoCommandY_00 has them. The diamond's dark backing (P_Base*) leaves the lit place open: two textures,
# open at the right (Nt_KeyBtnSubBaseAL_00) or at the bottom (...BL_00), turned 180 degrees (and mirrored) for the left
# and the top, as the four parts have them. Its animations scale the diamond and colour the window, so nothing moves back.
LAYOUT, LAYOUT_PACK = 'Layout/Common.sblarc', 'Pack/Bootup.pack'
PARTS = {s: f'blyt/PaDoCommand{s.upper()}_00.bflyt' for s in 'abxy'}
HOME = {'a': 'right', 'b': 'bottom', 'x': 'top', 'y': 'left'}
BUTTON = {'right': (26.0, 0.0), 'bottom': (0.0, -26.0), 'top': (0.0, 26.0), 'left': (-26.0, 0.0)}
LETTER = {'right': (26.2, 0.8), 'bottom': (1.0, -26.0), 'top': (0.8, 26.0), 'left': (-25.5, -0.2)}
LABEL = {'bottom': 'A', 'right': 'B', 'left': 'X', 'top': 'Y'}
BASE = {'right': ('A', 1.0, 0.0), 'bottom': ('B', 1.0, 0.0), 'top': ('B', -1.0, 180.0), 'left': ('A', 1.0, 180.0)}


def flyt_panes(flyt):
    """[(magic, offset, name, parent index or -1)] of a BFLYT's panes in file order."""
    assert flyt[:4] == b'FLYT', 'not a BFLYT'
    k, panes, stack = struct.unpack_from('<H', flyt, 6)[0], [], [-1]
    for _ in range(struct.unpack_from('<H', flyt, 0x10)[0]):
        magic, size = flyt[k:k+4].decode(), struct.unpack_from('<I', flyt, k + 4)[0]
        if magic in ('pan1', 'pic1', 'txt1', 'wnd1', 'prt1', 'bnd1'):
            panes.append((magic, k, flyt[k+12:k+36].split(b'\0')[0].decode(), stack[-1]))
        elif magic == 'pas1':
            stack.append(len(panes) - 1)
        elif magic == 'pae1':
            stack.pop()
        k += size
    return panes


def diamond(flyt, home, slot):
    """flyt (a part whose button is at home) with that button at slot and the Xbox letters in the other three places."""
    panes, out = flyt_panes(flyt), bytearray(flyt)
    index = {name: i for i, (_, _, name, _) in enumerate(panes)}
    under = lambda p: [i for i, q in enumerate(panes) if q[3] == p]  # noqa: E731
    text = lambda k: k + struct.unpack_from('<I', flyt, k + 0x64)[0]  # noqa: E731
    loop = index['N_Loop_00']
    [button] = [i for i in under(loop) if panes[i][2].startswith('N_Btn')]
    letters = [i for i in under(loop) if panes[i][0] == 'txt1' and flyt[text(panes[i][1]):text(panes[i][1]) + 4] in
               (b'A\0\0\0', b'B\0\0\0', b'X\0\0\0', b'Y\0\0\0')]
    assert len(letters) == 3, f'{len(letters)} letters around the button'
    moved = [button] + [i for i in under(index['RootPane']) if panes[i][2].startswith(('N_Capture', 'N_Attention'))]
    mirror = (home == 'left') != (slot == 'left')
    (hx, hy), (sx, sy) = BUTTON[home], BUTTON[slot]
    for i in moved:
        k = panes[i][1]; tx, ty = struct.unpack_from('<2f', out, k + 0x2c)
        struct.pack_into('<2f', out, k + 0x2c, sx + (hx - tx if mirror else tx - hx), sy + ty - hy)
    for i in range(len(panes)) if mirror else ():
        p = panes[i][3]
        while p not in moved and p != -1:
            p = panes[p][3]
        if p == -1:
            continue
        magic, k = panes[i][:2]
        struct.pack_into('<f', out, k + 0x2c, -struct.unpack_from('<f', out, k + 0x2c)[0])
        if magic == 'wnd1':
            struct.pack_into('<f', out, k + 0x44, -struct.unpack_from('<f', out, k + 0x44)[0])
        if magic == 'txt1':                         # origin and text left <-> right (low two bits: 1 left, 2 right)
            for at in (k + 9, k + 0x5c):
                out[at] = out[at] & ~3 | {1: 2, 2: 1}.get(out[at] & 3, out[at] & 3)
    for i, spot in zip(letters, [s for s in ('right', 'bottom', 'top', 'left') if s != slot]):
        k = panes[i][1]
        struct.pack_into('<2f', out, k + 0x2c, *LETTER[spot]); out[text(k)] = ord(LABEL[spot])
    at = flyt.index(b'Nt_KeyTex', flyt.index(b'txl1')) + 9
    assert flyt[at:at + 5] in (b'A_00^', b'B_00^', b'X_00^', b'Y_00^'), 'no key texture'
    out[at] = ord(LABEL[slot])
    letter, scale, turn = BASE[slot]
    at = flyt.index(b'Nt_KeyBtnSubBase', flyt.index(b'txl1')) + 16
    assert flyt[at:at + 5] in (b'AL_00', b'BL_00'), 'no diamond texture'
    out[at] = ord(letter)
    [base] = [i for i in under(loop) if panes[i][2].startswith('P_Base')]
    k = panes[base][1]; struct.pack_into('<f', out, k + 0x40, turn); struct.pack_into('<f', out, k + 0x44, scale)
    return bytes(out)


def layout_remap(blarc, slots):
    """blarc with each part of slots ({switch letter: slot}) drawn by diamond(); same size."""
    out = bytearray(blarc)
    for s, slot in slots.items():
        a, b = sarc_member(blarc, PARTS[s]); out[a:b] = diamond(blarc[a:b], HOME[s], slot)
    return bytes(out)


def yaz0_patch(packed, old, new):
    """packed (Yaz0 of old) for new, a copy of old changed in place: the tokens that write a changed byte, or copy one,
    become literals, every other token stays, so the stream grows by a few hundred bytes instead of being redone."""
    import bisect
    changed = [i for i in range(len(old)) if old[i] != new[i]] if len(old) < 4096 else \
        [i for a in range(0, len(old), 4096) if old[a:a+4096] != new[a:a+4096] for i in range(a, min(a + 4096, len(old)))
         if old[i] != new[i]]
    touches = lambda lo, hi: (lambda j: j < len(changed) and changed[j] < hi)(bisect.bisect_left(changed, lo))  # noqa: E731
    out = bytearray(packed[:16]); group, flags, n = bytearray(), 0, 0
    def emit(literal, token):
        nonlocal group, flags, n
        flags |= literal << (7 - n); group += token; n += 1
        if n == 8:
            out.append(flags); out.extend(group); group, flags, n = bytearray(), 0, 0
    i, o, size = 16, 0, len(old)
    while o < size:
        code = packed[i]; i += 1
        for bit in range(8):
            if o >= size:
                break
            if code & (0x80 >> bit):
                emit(1, bytes((new[o],))); o += 1; i += 1
                continue
            first = packed[i]; dist = ((first & 0xF) << 8 | packed[i+1]) + 1; length = first >> 4
            raw = packed[i:i+2] if length else packed[i:i+3]; i += 2 if length else 3
            length = length + 2 if length else raw[2] + 0x12
            if touches(o, o + length) or touches(o - dist, o - dist + length):
                for b in new[o:o+length]:
                    emit(1, bytes((b,)))
            else:
                emit(0, raw)
            o += length
    if n:
        out.append(flags); out.extend(group)
    return bytes(out)


def yaz0_stored(data, alignment):
    """Yaz0 with stored blocks only: each code byte 0xFF announces eight literal bytes."""
    out = bytearray(b'Yaz0' + struct.pack('>II', len(data), alignment) + bytes(4))
    for i in range(0, len(data), 8):
        out += b'\xff' + data[i:i+8]
    return bytes(out)


def own(path, sarc):
    """Whether the archive at path is this script's: the game's sarc with only External_00.bffnt's CMAP table changed."""
    try:
        mine = yaz0(path.read_bytes())
    except (OSError, ValueError):
        return False
    a, b = sarc_member(sarc, MEMBER); table, lo, hi = cmap_table(sarc[a:b]); t0, t1 = a + table, a + table + (hi - lo + 1)*2
    return len(mine) == len(sarc) and mine[:t0] == sarc[:t0] and mine[t1:] == sarc[t1:]


def own_pack(path, pack, blarc):
    """Whether the pack at path is this script's: the game's pack with only the four parts of its layout archive changed."""
    try:
        mine = path.read_bytes(); layout = yaz0(mine[slice(*sarc_member(mine, LAYOUT))])
        others = sorted(set(pack_names(pack)) - {LAYOUT})
        if sorted(set(pack_names(mine)) - {LAYOUT}) != others or len(layout) != len(blarc) or any(
                mine[slice(*sarc_member(mine, n))] != pack[slice(*sarc_member(pack, n))] for n in others):
            return False
    except (OSError, ValueError, KeyError, AssertionError):
        return False
    edges = [0] + [x for p in sorted(sarc_member(blarc, p) for p in PARTS.values()) for x in p] + [len(blarc)]
    return all(layout[edges[i]:edges[i+1]] == blarc[edges[i]:edges[i+1]] for i in range(0, len(edges), 2))


def pack_names(sarc):
    nodes = struct.unpack_from('<H', sarc, 0x1a)[0]; names = 0x20 + nodes*16 + 8
    return [sarc[at:sarc.index(b'\0', at)].decode() for at in
            (names + (struct.unpack_from('<I', sarc, 0x20 + k*16 + 4)[0] & 0xffffff)*4 for k in range(nodes))]


def layout_plan(controls):
    """(glyphs, diamond) for the layout of a controls.txt (bytes, or None for the default layout), by position."""
    sys.path.insert(0, str(REPO))
    from tools.builder import controls_file, icons_plan
    parsed = controls_file.parse(controls or b'')
    if parsed.line:
        sys.exit(f'controls.txt line {parsed.line}: {parsed.error}')
    inputs, _ = controls_file.layout(parsed, False)
    return icons_plan.botw_glyphs(inputs), icons_plan.botw_diamond(inputs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder (for the app: <data>/BotW/mods/botw190)')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path, help='the built Breath of the Wild.app')
    source.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and runtime-index.bin')
    parser.add_argument('--version', default='botw190', choices=('botw190',), help='the game version (default botw190)')
    parser.add_argument('--controls', type=Path, default=DATA/'controls.txt',
                        help='the layout (default: the game\'s controls.txt; a missing file is the default layout)')
    parser.add_argument('--plan', type=Path, help='JSON {"glyphs": [[code point, code point shown], ...], "diamond": '
                        '{switch letter: bottom|right|left|top}} (the builder\'s plan; replaces --controls)')
    parser.add_argument('--write-all', action='store_true', help='write every archive, also one whose icons the layout '
                        'leaves as they are (the builder: its icons mod must replace something to be valid)')
    args = parser.parse_args()
    romfs = args.romfs
    if args.app:
        romfs = args.app/f'Contents/Resources/totk/extracted/{args.version}/romfs'
        if not (romfs/'romfs.bin').exists():   # built with --mac-game-data library
            make_mod_manifest.IMAGE_DIR = app_image_dir(args.app, args.version)
    for need in (image_file(romfs), romfs/'runtime-index.bin'):
        if not need.is_file():
            parser.error(f'{need} not found')
    if args.plan:
        plan = json.loads(args.plan.read_text())
        glyphs, slots = {cp: shown for cp, shown in plan['glyphs']}, plan['diamond']
    else:
        kind = args.controls.with_name('controller-type.txt')
        if kind.is_file() and kind.read_text().strip() == 'switch':
            sys.exit(f'{kind} says switch: a Nintendo controller\'s face buttons go by label, which this does not draw')
        glyphs, slots = layout_plan(args.controls.read_bytes() if args.controls.is_file() else None)
    assert set(slots) <= set(PARTS) and set(slots.values()) <= set(BUTTON), f'bad diamond plan {slots}'
    index = read_index(romfs/'runtime-index.bin')
    archives = sorted(p for p in index if p.startswith(ARCHIVES) and p.endswith('.sbfarc'))
    if not archives:
        sys.exit(f'no {ARCHIVES}*.sbfarc in {romfs}')
    if args.mod.is_dir():   # checked before anything is written: a refused run changes nothing
        misspelt = manifest(args.mod, romfs).misspelt
        if misspelt:
            sys.exit('nothing written: ' + ', '.join(f'{a} (the RomFS has {b})' for a, b in misspelt))
    work = []
    with open(image_file(romfs), 'rb') as image:
        for path in archives:
            offset, size = index[path]; image.seek(offset); packed = image.read(size)
            sarc = yaz0(packed); a, b = sarc_member(sarc, MEMBER)
            if (args.mod/path).exists() and not own(args.mod/path, sarc):
                sys.exit(f'nothing written: {args.mod/path} is another mod\'s font archive (move it away first)')
            work.append((path, packed, sarc, sarc[:a] + remap(sarc[a:b], glyphs) + sarc[b:]))
        if LAYOUT_PACK in index:
            offset, size = index[LAYOUT_PACK]; image.seek(offset); pack = image.read(size)
            a, b = sarc_member(pack, LAYOUT); blarc = yaz0(pack[a:b]); guide = layout_remap(blarc, slots)
            if (args.mod/LAYOUT_PACK).exists() and not own_pack(args.mod/LAYOUT_PACK, pack, blarc):
                sys.exit(f'nothing written: {args.mod/LAYOUT_PACK} is another mod\'s pack (move it away first)')
            changed = pack if guide == blarc else sarc_put(pack, LAYOUT, yaz0_patch(pack[a:b], blarc, guide))
            work.append((LAYOUT_PACK, None, pack, changed))
    args.mod.mkdir(parents=True, exist_ok=True)
    for path, packed, sarc, changed in work:
        out = args.mod/path
        if changed == sarc and not args.write_all:
            out.unlink(missing_ok=True); continue
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(changed if packed is None else yaz0_stored(changed, struct.unpack_from('>I', packed, 8)[0]))
    found = write_manifest(args.mod, romfs)
    if found.misspelt:   # a file added while this ran
        sys.exit('manifest not written: ' + ', '.join(f'{a} (the RomFS has {b})' for a, b in found.misspelt))
    moved = ', '.join(f'U+{cp:X} -> U+{shown:X}' for cp, shown in sorted(glyphs.items())) or 'none (the prompts already name these buttons)'
    lit = ', '.join(f'{s.upper()} {slot}' for s, slot in sorted(slots.items())) or 'none'
    print(f'{args.mod}: {len(archives)} font archives; icons moved: {moved}; action guide: {lit}')


if __name__ == '__main__':
    main()
