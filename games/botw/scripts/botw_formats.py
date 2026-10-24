"""File formats the Breath of the Wild prompt tool reads and writes (botw_prompts.py): Yaz0 with a local re-encode, BNTX
textures (Switch block-linear R8 and R8G8) and the glyph rasterizer. Plain Python, no game data, no fontTools.

Yaz0 is the game's compression. A compressed archive is changed by decoding it, editing bytes in place and writing the stream
back with yaz0_splice(): only the stretch around the edit is encoded again (as literals), everything before and after it keeps
the original stream, so a few kilobytes of texture cost a few kilobytes, not a recompression of 33 MB.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'totk' / 'scripts'))   # the shared Yaz0 reader and SARC tools
from prepare_shader_seed import yaz0 as yaz0_decode     # noqa: E402,F401
from make_romfs_patches import sarc_members             # noqa: E402,F401
from sarc_repack import repack as sarc_replace          # noqa: E402,F401

REACH = 4096        # a Yaz0 back reference reaches this far back
YAZ0_HEADER = 16


# ---- Yaz0 -------------------------------------------------------------------------------------------------------------
def yaz0_groups(data):
    """(stream offset, output position) at the start of every group (a code byte and its eight items) of a Yaz0 stream, plus
    one entry for the end of the stream."""
    size = struct.unpack_from('>I', data, 4)[0]
    out, i, groups = 0, YAZ0_HEADER, []
    while out < size:
        groups.append((i, out))
        code = data[i]; i += 1
        for bit in (0x80, 0x40, 0x20, 0x10, 8, 4, 2, 1):
            if out >= size:
                break
            if code & bit:
                out += 1; i += 1
            else:
                length = data[i] >> 4; i += 2
                if length:
                    length += 2
                else:
                    length = data[i] + 0x12; i += 1
                out += length
    groups.append((i, size))
    return groups


def yaz0_literals(chunk):
    """The bytes of `chunk` as Yaz0 groups of eight literals."""
    out = bytearray()
    for i in range(0, len(chunk), 8):
        block = chunk[i:i + 8]
        out.append((0xFF00 >> len(block)) & 0xFF)
        out += block
    return bytes(out)


def full_edits(edits):
    """`edits` as (offset, old length, new bytes): an edit given as (offset, new bytes) keeps the size."""
    return sorted((e[0], len(e[1]), e[1]) if len(e) == 2 else tuple(e) for e in edits)


def apply_edits(data, edits):
    """`data` with `edits` ((offset, new bytes) or (offset, old length, new bytes), offsets in `data`, not overlapping) applied."""
    out, at = bytearray(), 0
    for offset, old, blob in full_edits(edits):
        if offset < at:
            raise ValueError('edits overlap')
        out += data[at:offset]; out += blob; at = offset + old
    return bytes(out + data[at:])


def yaz0_splice(data, decoded, edits):
    """The Yaz0 stream `data` (which decodes to `decoded`) with `edits` applied: a list of (offset, new bytes) that keep the
    size, or (offset, old length, new bytes) that may change it (offsets in `decoded`). The stream before the first group at or
    before an edit and after the first group that starts REACH bytes past it is kept as it was (a back reference only reaches
    unchanged bytes there, so it decodes the same after a shift); the stretch between is written as literals. Decodes the result
    and checks it before returning it."""
    if not edits:
        return data
    edits = full_edits(edits)
    expected = apply_edits(decoded, edits)
    spans = []      # merged [lo, hi, size change] of the edited bytes, edits closer than REACH together
    for lo, old, blob in edits:
        if spans and lo - spans[-1][1] <= REACH:
            spans[-1][1] = max(spans[-1][1], lo + old); spans[-1][2] += len(blob) - old
        else:
            spans.append([lo, lo + old, len(blob) - old])
    groups = yaz0_groups(data)
    out, taken, shift = bytearray(data[:YAZ0_HEADER]), 0, 0    # `taken`: stream offset up to which the original is copied;
    while spans:                                                # `shift`: how far the output has moved by earlier edits
        lo, hi, change = spans.pop(0)
        before = [g for g in groups[:-1] if g[1] <= lo and g[0] >= taken]     # the last group may be short: never start after it
        if not before:
            raise ValueError('an edit starts inside an earlier stretch')
        start = before[-1]
        while True:
            # The literals fill whole groups of eight, so the stretch must be a multiple of 8 bytes (or reach the stream's end).
            after = [g for g in groups if g[1] >= hi + REACH and ((g[1] - start[1] + change) % 8 == 0 or g is groups[-1])]
            end = after[0] if after else groups[-1]
            if spans and spans[0][0] < end[1]:     # the stretch runs over the next edit: it becomes part of this stretch
                _, more_hi, more = spans.pop(0)
                hi, change = max(hi, more_hi), change + more
                continue
            break
        out += data[taken if taken else YAZ0_HEADER:start[0]]
        out += yaz0_literals(bytes(expected[start[1] + shift:end[1] + shift + change]))
        taken, shift = end[0], shift + change
    out += data[taken:]
    struct.pack_into('>I', out, 4, len(expected))
    result = bytes(out)
    if yaz0_decode(result) != bytes(expected):
        raise ValueError('the re-encoded Yaz0 stream does not decode to the intended bytes')
    return result


def sarc_member_edits(sarc, name, edits):
    """Edits of the SARC `sarc` (offsets in it) for `edits` of its member `name` (offsets in the member): the member's edits
    moved to its place, and the SARC's own header (member ranges, file size) as sarc_replace() writes it. A member that grows by
    a multiple of GROW_ALIGN leaves the members behind it on the same alignment, so they only move; checked against
    sarc_replace()."""
    lo, hi = sarc_members(sarc)[name]
    moved = [(lo + o, n, b) for o, n, b in full_edits(edits)]
    whole = sarc_replace(sarc, name, apply_edits(sarc[lo:hi], edits))
    start = struct.unpack_from('<I', sarc, 12)[0]
    out = [(0, start, whole[:start])] + moved
    if apply_edits(sarc, out) != whole:
        raise ValueError(f'{name}: the archive does not keep its layout when this member grows')
    return out


# ---- RSTB (System/Resource/ResourceSizeTable.product.srsizetable: the memory the game sets aside for each resource) --------
RSTB = 'System/Resource/ResourceSizeTable.product.srsizetable'


def rstb_sizes(table, names):
    """The sizes the decoded RSTB `table` lists for the resource `names`, as name -> size. A name it does not list by its CRC32
    is refused."""
    import zlib
    if table[:4] != b'RSTB':
        raise ValueError('not an RSTB')
    crcs = struct.unpack_from('<I', table, 4)[0]
    at = dict(struct.unpack_from('<II', table, 12 + 8 * i) for i in range(crcs))
    sizes = {}
    for name in names:
        if zlib.crc32(name.encode()) not in at:
            raise ValueError(f'{name}: not in the resource size table')
        sizes[name] = at[zlib.crc32(name.encode())]
    return sizes


def rstb_edits(table, growth):
    """Same-size edits of the decoded RSTB `table` that raise the entries of `growth` (resource name, e.g. Layout/Common.blarc,
    -> bytes it grew) by that much. A name the table does not list by its CRC32 is refused."""
    import zlib
    if table[:4] != b'RSTB':
        raise ValueError('not an RSTB')
    crcs, names = struct.unpack_from('<II', table, 4)
    at = {struct.unpack_from('<I', table, 12 + 8 * i)[0]: 12 + 8 * i for i in range(crcs)}
    edits = []
    for name, grow in sorted(growth.items()):
        entry = at.get(zlib.crc32(name.encode()))
        if entry is None:
            raise ValueError(f'{name}: not in the resource size table')
        edits.append((entry + 4, struct.pack('<I', struct.unpack_from('<I', table, entry + 4)[0] + grow)))
    return edits


# ---- BFLYT ------------------------------------------------------------------------------------------------------------


PANE_MAGICS = (b'pan1', b'pic1', b'txt1', b'wnd1', b'bnd1', b'prt1', b'ali1', b'scr1')
PANE_TRANSLATION = 44       # a pane section's (x, y, z) floats start this far into the section


def bflyt_panes(blob):
    """The panes of a layout (FLYT) as (section offset, name, parent name or None) in file order: a pas1 section opens the
    children of the pane before it and a pae1 closes them. A pane's translation (x, y) is the two floats at
    offset + PANE_TRANSLATION."""
    if blob[:4] != b'FLYT':
        raise ValueError('not a bflyt')
    head, count = struct.unpack_from('<H', blob, 6)[0], struct.unpack_from('<H', blob, 0x10)[0]
    pos, stack, last, panes = head, [], None, []
    for _ in range(count):
        magic, size = bytes(blob[pos:pos + 4]), struct.unpack_from('<I', blob, pos + 4)[0]
        if magic in PANE_MAGICS:
            last = bytes(blob[pos + 12:pos + 36]).split(b'\0')[0].decode()
            panes.append((pos, last, stack[-1] if stack else None))
        elif magic == b'pas1':
            stack.append(last)
        elif magic == b'pae1' and stack:
            stack.pop()
        pos += size
    return panes


# ---- BNTX -------------------------------------------------------------------------------------------------------------
R8_UNORM = 0x201
R8G8_UNORM = 0x901      # two bytes a pixel: grey shading, then alpha (the controller pictures)
BC4_UNORM = 0x1d01
TILE_BLOCK_LINEAR = 0


class Texture:
    """One BRTI of a BNTX: where its pixels are and how they are laid out."""
    def __init__(self, bntx, brti):
        (self.tile_mode, _, self.mips, _, self.format, _, self.width, self.height, self.depth, self.layers,
         layout) = struct.unpack_from('<HHHIIIIIIII', bntx, brti + 0x12)
        self.block_height = 1 << (layout & 7)
        self.image_size = struct.unpack_from('<I', bntx, brti + 0x50)[0]
        name = struct.unpack_from('<Q', bntx, brti + 0x60)[0]
        self.name = bntx[name + 2:name + 2 + struct.unpack_from('<H', bntx, name)[0]].decode()
        pointers = struct.unpack_from('<Q', bntx, brti + 0x70)[0]
        self.offset = struct.unpack_from('<Q', bntx, pointers)[0]


def bntx_textures(bntx):
    """name -> Texture for every texture of a BNTX."""
    if bntx[:4] != b'BNTX':
        raise ValueError('not a BNTX')
    return {t.name: t for t in (Texture(bntx, m.start()) for m in re.finditer(b'BRTI', bntx))}


def gob_address(x, y, width, element, block_height):
    """Byte offset of element (x, y) in a block-linear (GOB) texture whose elements are `element` bytes (a pixel, or a 4x4 block
    of a compressed format); `width` is in elements."""
    xb = x * element
    gobs_wide = (width * element + 63) // 64
    gob = (y // (8 * block_height)) * 512 * block_height * gobs_wide + (xb // 64) * 512 * block_height + (y % (8 * block_height) // 8) * 512
    return gob + ((xb % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((xb % 32) // 16) * 32 + (y % 2) * 16 + (xb % 16)


def r8_address(x, y, width, block_height):
    """Byte offset of pixel (x, y) in a one-byte-per-pixel block-linear texture."""
    return gob_address(x, y, width, 1, block_height)


def check_r8(texture):
    if (texture.format, texture.tile_mode, texture.mips, texture.layers, texture.depth) != (R8_UNORM, TILE_BLOCK_LINEAR, 1, 1, 1):
        raise ValueError(f'{texture.name}: expected a plain single-mip R8 block-linear texture, got format 0x{texture.format:x}, '
                         f'tile mode {texture.tile_mode}, {texture.mips} mips, {texture.layers} layers')


def r8_read(bntx, texture):
    """The pixels of an R8 texture as rows of bytes (list of bytes, top row first)."""
    check_r8(texture)
    base, w = texture.offset, texture.width
    return [bytes(bntx[base + r8_address(x, y, w, texture.block_height)] for x in range(w)) for y in range(texture.height)]


def r8_image(bntx, texture, rows):
    """The texture's image bytes (image_size of them, from the original) with the pixels replaced by `rows`; the padding bytes
    stay as they were."""
    check_r8(texture)
    if len(rows) != texture.height or any(len(r) != texture.width for r in rows):
        raise ValueError(f'{texture.name}: {texture.width}x{texture.height} pixels expected')
    out = bytearray(bntx[texture.offset:texture.offset + texture.image_size])
    for y, row in enumerate(rows):
        for x, value in enumerate(row):
            out[r8_address(x, y, texture.width, texture.block_height)] = value
    return bytes(out)


def check_rg8(texture):
    if (texture.format, texture.tile_mode, texture.mips, texture.layers, texture.depth) != (R8G8_UNORM, TILE_BLOCK_LINEAR, 1, 1, 1):
        raise ValueError(f'{texture.name}: expected a plain single-mip R8G8 block-linear texture, got format 0x{texture.format:x}, '
                         f'tile mode {texture.tile_mode}, {texture.mips} mips, {texture.layers} layers')


def rg8_read(bntx, texture):
    """The pixels of an R8G8 texture as two pictures (grey rows, alpha rows), each a list of bytes, top row first."""
    check_rg8(texture)
    base, w = texture.offset, texture.width
    at = [[base + gob_address(x, y, w, 2, texture.block_height) for x in range(w)] for y in range(texture.height)]
    return [bytes(bntx[a] for a in row) for row in at], [bytes(bntx[a + 1] for a in row) for row in at]


def rg8_image(bntx, texture, grey, alpha):
    """The texture's image bytes (image_size of them, from the original) with the pixels replaced by the `grey` and `alpha` rows;
    the padding bytes stay as they were."""
    check_rg8(texture)
    for rows in (grey, alpha):
        if len(rows) != texture.height or any(len(r) != texture.width for r in rows):
            raise ValueError(f'{texture.name}: {texture.width}x{texture.height} pixels expected')
    out = bytearray(bntx[texture.offset:texture.offset + texture.image_size])
    for y in range(texture.height):
        for x in range(texture.width):
            at = gob_address(x, y, texture.width, 2, texture.block_height)
            out[at], out[at + 1] = grey[y][x], alpha[y][x]
    return bytes(out)


def block_height_for(height):
    """The GOB block height (in GOBs of 8 rows) the Switch picks for a texture `height` elements (pixels or 4x4 blocks) high."""
    bh = 1
    while bh < 16 and bh * 8 < height:
        bh *= 2
    return bh


def surface_size(width, height, element, block_height):
    """Bytes of a block-linear texture of width x height elements of `element` bytes."""
    gobs_wide = (width * element + 63) // 64
    return gobs_wide * 512 * block_height * ((height + 8 * block_height - 1) // (8 * block_height))


def r8_pack(rows):
    """(image bytes, block height) of a new block-linear R8 texture from `rows` (any size)."""
    w, h = len(rows[0]), len(rows); bh = block_height_for(h)
    out = bytearray(surface_size(w, h, 1, bh))
    for y, row in enumerate(rows):
        for x, value in enumerate(row):
            out[gob_address(x, y, w, 1, bh)] = value
    return bytes(out), bh


def rg8_pack(grey, alpha):
    """(image bytes, block height) of a new block-linear R8G8 texture from the `grey` and `alpha` rows."""
    w, h = len(grey[0]), len(grey); bh = block_height_for(h)
    out = bytearray(surface_size(w, h, 2, bh))
    for y in range(h):
        for x in range(w):
            at = gob_address(x, y, w, 2, bh)
            out[at], out[at + 1] = grey[y][x], alpha[y][x]
    return bytes(out), bh


def bc4_pack(rows):
    """(image bytes, block height) of a new block-linear BC4 texture from `rows` (width and height multiples of 4)."""
    w, h = len(rows[0]) // 4, len(rows) // 4; bh = block_height_for(h)
    out = bytearray(surface_size(w, h, 8, bh))
    for by in range(h):
        for bx in range(w):
            at = gob_address(bx, by, w, 8, bh)
            out[at:at + 8] = bc4_encode([rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)])
    return bytes(out), bh


GROW_ALIGN = 0x2000     # a grown BNTX grows by a multiple of this: what follows it in a SARC or a BFFNT keeps its alignment


def bntx_resize(bntx, images):
    """Edits ((offset, old length, new bytes), offsets in `bntx`) that give textures a new size: `images` maps a texture name
    to (width, height, image bytes, block height) in the texture's own format. The new pixels go after the texture data (BRTD,
    each 0x200 aligned) and the relocation table (_RLT, at the end) moves behind them; the texture's BRTI (size, block height,
    image size), its data pointer, the BRTD and its relocation section sizes, and the file header follow. The old pixels stay
    unused where they were."""
    textures = bntx_textures(bntx)
    rlt, size = struct.unpack_from('<II', bntx, 0x18)
    brtd = struct.unpack_from('<Q', bntx, 0x30)[0]          # the NX block's pointer to the BRTD
    if bntx[rlt:rlt + 4] != b'_RLT' or bntx[brtd:brtd + 4] != b'BRTD' or size != len(bntx):
        raise ValueError('unexpected BNTX layout')
    brtd_size = struct.unpack_from('<Q', bntx, brtd + 8)[0]
    if brtd + brtd_size != rlt:
        raise ValueError('the BNTX texture data does not end at its relocation table')
    sections = struct.unpack_from('<I', bntx, rlt + 8)[0]
    data_section = [i for i in range(sections) if struct.unpack_from('<I', bntx, rlt + 0x18 + 24 * i)[0] == brtd]
    if len(data_section) != 1:
        raise ValueError('no relocation section for the BNTX texture data')
    blob, edits = bytearray(), []
    for name, (width, height, image, bh) in sorted(images.items()):
        texture = textures[name]
        brti = next(m.start() for m in re.finditer(b'BRTI', bntx) if Texture(bntx, m.start()).name == name)
        while (rlt + len(blob)) % 0x200:
            blob += b'\0'
        at = rlt + len(blob); blob += image
        layout = struct.unpack_from('<I', bntx, brti + 0x34)[0] & ~7 | (bh.bit_length() - 1)
        edits += [(brti + 0x24, 8, struct.pack('<II', width, height)), (brti + 0x34, 4, struct.pack('<I', layout)),
                  (brti + 0x50, 4, struct.pack('<I', len(image))),
                  (struct.unpack_from('<Q', bntx, brti + 0x70)[0], 8, struct.pack('<Q', at))]
        if texture.mips != 1:
            raise ValueError(f'{name}: only a single-mip texture can be resized')
    blob += bytes(-len(blob) % GROW_ALIGN)
    grow = len(blob)
    section = rlt + 0x10 + 24 * data_section[0]
    edits += [(0x18, 8, struct.pack('<II', rlt + grow, size + grow)), (brtd + 8, 8, struct.pack('<Q', brtd_size + grow)),
              (rlt, 0, bytes(blob)), (rlt + 4, 4, struct.pack('<I', rlt + grow)),
              (section + 12, 4, struct.pack('<I', struct.unpack_from('<I', bntx, section + 12)[0] + grow))]
    return edits


# ---- rasterizer -------------------------------------------------------------------------------------------------------
def flatten(operations, steps=12):
    """Contours (lists of points) from fontTools pen operations [(name, args), ...]; curves become `steps` line pieces."""
    contours, current, last = [], [], None
    for name, args in operations:
        if name == 'moveTo':
            current, last = [args[0]], args[0]
        elif name == 'lineTo':
            current.append(args[0]); last = args[0]
        elif name == 'curveTo':
            p0, (p1, p2, p3) = last, args[:3]
            for i in range(1, steps + 1):
                t = i / steps; u = 1 - t
                current.append((u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                                u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1]))
            last = p3
        elif name == 'qCurveTo':
            points = list(args)
            if points[-1] is None:
                raise ValueError('a quadratic contour without an on-curve point is not supported')
            pending, p0 = points[:-1], last
            while pending:      # implied on-curve points between consecutive off-curve points
                c = pending.pop(0)
                end = points[-1] if not pending else ((c[0] + pending[0][0]) / 2, (c[1] + pending[0][1]) / 2)
                for i in range(1, steps + 1):
                    t = i / steps; u = 1 - t
                    current.append((u * u * p0[0] + 2 * u * t * c[0] + t * t * end[0], u * u * p0[1] + 2 * u * t * c[1] + t * t * end[1]))
                p0 = end
            last = points[-1]
        elif name in ('closePath', 'endPath'):
            if current:
                contours.append(current)
            current = []
    return contours


def coverage_rect(contours, width, height, oversample=8):
    """Coverage 0-255 of a shape over a width x height picture. `contours` are in pixels, x right and y down (nonzero winding: a
    contour wound the other way cuts a hole). Returns rows of bytes, top row first."""
    edges = []
    for contour in contours:
        for (ax, ay), (bx, by) in zip(contour, contour[1:] + contour[:1]):
            if ay != by:
                edges.append((min(ay, by), max(ay, by), ax, ay, bx, by))
    n = oversample
    rows = []
    for py in range(height):
        near = [e for e in edges if e[0] < py + 1 and e[1] >= py]
        if not near:
            rows.append(bytes(width))
            continue
        steps = [0] * (width * n + 1)       # the change of the hit count at each sub-pixel; summed up below
        for sy in range(n):
            y = py + (sy + 0.5) / n
            crossings = sorted((ax + (y - ay) * (bx - ax) / (by - ay), 1 if by > ay else -1)
                               for lo, hi, ax, ay, bx, by in near if (ay <= y < by) or (by <= y < ay))
            winding, previous = 0, 0.0
            for x, direction in crossings:
                if winding:
                    start, stop = max(0, round(previous * n)), min(width * n, round(x * n))
                    if stop > start:
                        steps[start] += 1; steps[stop] -= 1
                winding += direction; previous = x
        hits, total = [], 0
        for step in steps[:-1]:
            total += step; hits.append(total)
        rows.append(bytes(min(255, round(255 * sum(hits[px * n:(px + 1) * n]) / (n * n))) for px in range(width)))
    return rows


def coverage(contours, size, oversample=8):
    """coverage_rect() over a square size x size picture."""
    return coverage_rect(contours, size, size, oversample)


def enclosed_marks(rows, window=(8, 8, 40, 40), dark=0xE0, largest=400):
    """The dark marks (a letter cut out of a white shape): connected groups of pixels below `dark` that lie inside `window`
    (x0, y0, x1, y1) and have at most `largest` pixels. Returns a list of pixel-coordinate sets; the outline of a shape is
    longer than `largest` and stays out."""
    height, width = len(rows), len(rows[0])
    seen, marks = set(), []
    for y in range(height):
        for x in range(width):
            if rows[y][x] >= dark or (x, y) in seen:
                continue
            group, todo = set(), [(x, y)]
            while todo:
                px, py = todo.pop()
                if (px, py) in group or not (0 <= px < width and 0 <= py < height) or rows[py][px] >= dark:
                    continue
                group.add((px, py))
                todo += [(px + dx, py + dy) for dx in (-1, 0, 1) for dy in (-1, 0, 1) if dx or dy]
            seen |= group
            xs, ys = [p[0] for p in group], [p[1] for p in group]
            if len(group) <= largest and min(xs) >= window[0] and min(ys) >= window[1] and max(xs) < window[2] and max(ys) < window[3]:
                marks.append(group)
    return marks


# ---- BC4 (one-channel block compression: 8 bytes for a 4x4 block) and the glyph sheet of a BFFNT ----------------------------
def bc4_decode(block):
    r0, r1 = block[0], block[1]
    if r0 > r1:
        palette = [r0, r1] + [((7 - i) * r0 + i * r1) // 7 for i in range(1, 7)]
    else:
        palette = [r0, r1] + [((5 - i) * r0 + i * r1) // 5 for i in range(1, 5)] + [0, 255]
    bits = int.from_bytes(block[2:8], 'little')
    return [palette[(bits >> (3 * i)) & 7] for i in range(16)]


def bc4_encode(values):
    """An 8-byte BC4 block for 16 values (the 8-colour mode, endpoints at the extremes, each value to the nearest entry)."""
    hi, lo = max(values), min(values)
    if hi == lo:
        return bytes([hi, lo]) + bytes(6) if hi else bytes(8)
    palette = [hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)]
    bits = 0
    for i, v in enumerate(values):
        bits |= min(range(8), key=lambda k: abs(palette[k] - v)) << (3 * i)
    return bytes([hi, lo]) + bits.to_bytes(6, 'little')


def check_bc4(texture):
    if (texture.format, texture.tile_mode, texture.mips, texture.layers, texture.depth) != (BC4_UNORM, TILE_BLOCK_LINEAR, 1, 1, 1):
        raise ValueError(f'{texture.name or "texture"}: expected a single-mip BC4 block-linear texture, got format 0x{texture.format:x}')


def bc4_read(blob, texture):
    """The pixels of a BC4 texture as a list of bytearray rows, top row first."""
    check_bc4(texture)
    rows = [bytearray(texture.width) for _ in range(texture.height)]
    for by in range(texture.height // 4):
        for bx in range(texture.width // 4):
            at = texture.offset + gob_address(bx, by, texture.width // 4, 8, texture.block_height)
            for i, v in enumerate(bc4_decode(blob[at:at + 8])):
                rows[by * 4 + i // 4][bx * 4 + i % 4] = v
    return rows


def bc4_image(blob, texture, rows, boxes):
    """The texture's image bytes with only the 4x4 blocks that touch one of `boxes` ((x0, y0, x1, y1) each) written again from
    `rows`; every other block keeps its original bytes."""
    check_bc4(texture)
    out = bytearray(blob[texture.offset:texture.offset + texture.image_size])
    for x0, y0, x1, y1 in boxes:
        for by in range(y0 // 4, (y1 + 3) // 4):
            for bx in range(x0 // 4, (x1 + 3) // 4):
                at = gob_address(bx, by, texture.width // 4, 8, texture.block_height)
                out[at:at + 8] = bc4_encode([rows[by * 4 + i // 4][bx * 4 + i % 4] for i in range(16)])
    return bytes(out)


class Bffnt:
    """The glyph sheet of a BFFNT (the bitmap fonts of Font/Font_*.sbfarc): a BNTX with one BC4 texture inside the TGLP section,
    a grid of cells, and a CMAP that gives each code point its cell. A cell is `cell` pixels wide and `cell_height` high (1.9.0:
    50x50 in 5 x 20 on a 256x1024 sheet; 1.6.0: 44x39 in 2 x 25 on 128x1024). Upright, the grid starts at the sheet's top left
    corner with a one-pixel gap before each cell; the spare rows and columns are at the right and the bottom."""
    def __init__(self, data):
        if data[:4] != b'FFNT':
            raise ValueError('not a BFFNT')
        position, self.sections = struct.unpack_from('<H', data, 6)[0], {}
        for _ in range(struct.unpack_from('<H', data, 16)[0]):
            self.sections[data[position:position + 4]] = position
            position += struct.unpack_from('<I', data, position + 4)[0]
        tglp = self.sections[b'TGLP']
        self.cell, self.cell_height, self.sheets, _, size, _, _, self.columns, self.rows, _, _, sheet = struct.unpack_from('<BBBBIHHHHHHI', data, tglp + 8)
        if self.sheets != 1:
            raise ValueError('a BFFNT with more than one sheet is not supported')
        self.sheet_at = sheet
        blob = data[sheet:sheet + size]
        textures = bntx_textures(blob)
        if len(textures) != 1:
            raise ValueError('expected one texture in the glyph sheet')
        self.texture = next(iter(textures.values()))
        cmap = self.sections[b'CMAP']
        begin, end, kind = struct.unpack_from('<IIH', data, cmap + 8)
        if kind != 1:
            raise ValueError('only a CMAP of kind 1 (a table) is supported')
        self.cells = {begin + i: v for i, v in enumerate(struct.unpack_from(f'<{end - begin + 1}H', data, cmap + 24)) if v != 0xFFFF}
        self.data = data

    def get(self, rows, code):
        """The glyph cell of `code` as upright rows of bytes (the sheet is stored upside down, cell by cell)."""
        x0, y0, x1, y1 = self.cell_box(code)
        return [bytes(r[x0:x1]) for r in reversed(rows[y0:y1])]

    def put(self, rows, code, cell):
        """Write an upright cell (`cell` rows of bytes) into the sheet's rows; returns the box it covers."""
        x0, y0, x1, y1 = self.cell_box(code)
        if len(cell) != y1 - y0 or any(len(r) != x1 - x0 for r in cell):
            raise ValueError(f'a glyph cell is {x1 - x0}x{y1 - y0} pixels')
        for y, line in zip(range(y0, y1), reversed(cell)):
            rows[y][x0:x1] = line
        return (x0, y0, x1, y1)

    def grow_sheet(self, rows):
        """Edits ((offset, old length, new bytes), offsets in the BFFNT) that replace the glyph sheet by `rows`, a larger picture
        of the same grid (scale times as wide and high: every cell and its one-pixel gap scaled). The TGLP keeps its cell and
        sheet sizes (the game takes them as the size on screen, and finds a cell's place as a fraction of the sheet), so only the
        sheet's texture, its byte size and the places of the sections behind it change."""
        data, tglp = self.data, self.sections[b'TGLP']
        sheet_size = struct.unpack_from('<I', data, tglp + 12)[0]
        sheet = data[self.sheet_at:self.sheet_at + sheet_size]
        image, bh = bc4_pack(rows)
        edits = [(self.sheet_at + o, n, b) for o, n, b in bntx_resize(sheet, {self.texture.name: (len(rows[0]), len(rows), image, bh)})]
        grow = sum(len(b) - n for _, n, b in edits)
        after = self.sheet_at + sheet_size       # pointers past the sheet move by `grow`
        moved = lambda at: (at, 4, struct.pack('<I', struct.unpack_from('<I', data, at)[0] + grow))
        edits += [moved(0x0c), moved(tglp + 4), moved(tglp + 12)]
        finf = self.sections[b'FINF']
        edits += [moved(finf + k) for k in (20, 24, 28) if struct.unpack_from('<I', data, finf + k)[0] >= after]
        position = struct.unpack_from('<H', data, 6)[0]
        for _ in range(struct.unpack_from('<H', data, 16)[0]):
            magic, size = data[position:position + 4], struct.unpack_from('<I', data, position + 4)[0]
            link = {b'CWDH': 12, b'CMAP': 20}.get(magic)
            if link and struct.unpack_from('<I', data, position + link)[0] >= after:
                edits.append(moved(position + link))
            position += size
        return edits

    def cell_box(self, code):
        """(x0, y0, x1, y1) of the glyph cell of `code` in the sheet's stored pixels (upside down: y0 counts from the bottom)."""
        i = self.cells[code]
        step, rise = self.cell + 1, self.cell_height + 1
        x0, y0 = 1 + i % self.columns * step, self.texture.height - (i // self.columns + 1) * rise
        return x0, y0, x0 + self.cell, y0 + self.cell_height
