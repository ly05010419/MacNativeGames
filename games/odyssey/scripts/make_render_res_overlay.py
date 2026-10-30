#!/usr/bin/env python3
"""Build the RomFS overlay a bigger docked render size needs (TOTK_RENDER_RES=<w>x<h>, runtime/game_patches.c).

Super Mario Odyssey picks its docked scene size from SystemData/ProjectGraphicsQuality.szs (ProjectGraphicsQuality.byml,
"ProjectGraphicsQualityParamArray"): the "Console" entry of ParamType 0 is the largest size, 1600x900, and its dynamic
resolution steps down through the ParamType 2 entries of ranks 1..5 (1440x810 ... 800x450) when the GPU falls behind;
"SnapShot" (the photo mode) is 1600x900 too. Buffers the game sizes for the largest step (the light pre-pass among them)
come from these values, so a bigger scene needs them raised: raising only the scene size in code drew light in blocks
on the ground (2026-10-23). The stages' own GraphicsQuality.byml give ParamType 1 sizes relative to 1280x720 and
are left as they are.

This script scales the Console entries and SnapShot by <w>/1600 (rounded to even pixels; the height follows <h>/900)
and leaves Handheld, Vr and the rest. It edits the integers in place, so the byml, the SARC and every offset keep their
size, and writes the archive as Yaz0 with literal blocks only (valid for any Yaz0 reader).

Input: the IVFC-verified local RomFS image (extracted/local/romfs). Output: extracted/local/romfs-patches-out/<w>x<h>/
SystemData/ProjectGraphicsQuality.szs and manifest.tsv (path, size, SHA-256 of the overlay, SHA-256 of the original), the
format runtime/romfs.c reads (TOTK_ROMFS_PATCHES). Game content stays in the ignored work folder.
usage: make_render_res_overlay.py 1920x1080 [--romfs extracted/local/romfs] [--out extracted/local/romfs-patches-out]
"""
import argparse, hashlib, json, struct
from pathlib import Path

NAME = 'SystemData/ProjectGraphicsQuality.szs'
MEMBER = 'ProjectGraphicsQuality.byml'
SHIPPED = (1600, 900)


def yaz0_decode(data):
    assert data[:4] == b'Yaz0', 'not Yaz0'
    size = struct.unpack_from('>I', data, 4)[0]
    out = bytearray(); i = 16
    while len(out) < size:
        flags = data[i]; i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                out.append(data[i]); i += 1
            else:
                b1, b2 = data[i], data[i + 1]; i += 2
                dist = ((b1 & 0x0f) << 8 | b2) + 1
                n = b1 >> 4
                if n == 0:
                    n = data[i] + 0x12; i += 1
                else:
                    n += 2
                for _ in range(n):
                    out.append(out[-dist])
    return bytes(out)


def yaz0_store(data):
    """Yaz0 with every byte a literal: a flag byte 0xff before each 8 bytes."""
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        out.append((0xff00 >> len(chunk)) & 0xff if len(chunk) < 8 else 0xff)
        out += chunk
    return bytes(out)


def sarc_member(data, name):
    """(offset, size) of a member in a little-endian SARC."""
    assert data[:4] == b'SARC' and struct.unpack_from('<H', data, 6)[0] == 0xfeff, 'not a little-endian SARC'
    data_off = struct.unpack_from('<I', data, 0x0c)[0]
    sfat = struct.unpack_from('<H', data, 4)[0]
    count = struct.unpack_from('<H', data, sfat + 6)[0]
    sfnt = sfat + 12 + 16 * count
    names = sfnt + 8
    for k in range(count):
        attr, start, end = struct.unpack_from('<III', data, sfat + 12 + 16 * k + 4)
        if attr >> 24 != 1:
            continue
        noff = names + (attr & 0xffff) * 4
        if data[noff:data.index(b'\0', noff)].decode() == name:
            return data_off + start, end - start
    raise KeyError(name)


def quality_entries(b):
    """[(entry dict of name -> (type, value, offset of the value))] of the byml's ProjectGraphicsQualityParamArray."""
    assert b[:2] == b'YB', 'not a little-endian byml'
    key_off, str_off, root_off = struct.unpack_from('<III', b, 4)
    def strtab(off):
        n = struct.unpack_from('<I', b, off)[0] >> 8
        offs = struct.unpack_from(f'<{n + 1}I', b, off + 4)
        return [b[off + o:b.index(b'\0', off + o)].decode() for o in offs[:n]]
    keys, strs = strtab(key_off), strtab(str_off)
    def dict_entries(off):
        assert b[off] == 0xc1
        n = struct.unpack_from('<I', b, off)[0] >> 8
        out = {}
        for i in range(n):
            e = off + 4 + 8 * i
            k = struct.unpack_from('<I', b, e)[0] & 0xffffff
            out[keys[k]] = (b[e + 3], struct.unpack_from('<I', b, e + 4)[0], e + 4)
        return out
    root = dict_entries(root_off)
    t, arr, _ = root['ProjectGraphicsQualityParamArray']
    assert t == 0xc0 and b[arr] == 0xc0
    n = struct.unpack_from('<I', b, arr)[0] >> 8
    base = arr + 4 + ((n + 3) & ~3)
    entries = []
    for i in range(n):
        assert b[arr + 4 + i] == 0xc1
        e = dict_entries(struct.unpack_from('<I', b, base + 4 * i)[0])
        e['Name'] = (0xa0, strs[e['Name'][1]], e['Name'][2])
        entries.append(e)
    return entries


def even(x):
    return int(round(x / 2)) * 2


def scaled_table(byml, w, h):
    """The byml with Console and SnapShot sizes scaled to w x h; returns (bytes, [(name, rank, old, new)])."""
    b = bytearray(byml)
    changes = []
    for e in quality_entries(byml):
        name, rank = e['Name'][1], e['Rank'][1]
        if name not in ('Console', 'SnapShot'):
            continue
        (tw, ow, pw), (th, oh, ph) = e['ScreenWidthScale'], e['ScreenHeightScale']
        assert tw == 0xd1 and th == 0xd1, 'ScreenWidthScale / ScreenHeightScale are not integers'
        nw, nh = even(ow * w / SHIPPED[0]), even(oh * h / SHIPPED[1])
        struct.pack_into('<I', b, pw, nw); struct.pack_into('<I', b, ph, nh)
        changes.append((name, rank, (ow, oh), (nw, nh)))
    top = [c for c in changes if c[0] == 'Console' and c[2] == SHIPPED]
    assert len(top) == 1 and top[0][3] == (w, h), f'expected one Console entry of {SHIPPED[0]}x{SHIPPED[1]}, found {top}'
    return bytes(b), changes


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('size', help='<w>x<h>, e.g. 1920x1080 or 2560x1440')
    ap.add_argument('--romfs', type=Path, default=Path('extracted/local/romfs'))
    ap.add_argument('--out', type=Path, default=Path('extracted/local/romfs-patches-out'))
    ap.add_argument('--flat', action='store_true',
                    help='write into --out itself, not <out>/<w>x<h> (the app build: extracted/local/romfs-patches, loaded by default)')
    a = ap.parse_args()
    w, h = (int(x) for x in a.size.lower().split('x'))
    assert 640 <= w <= 3840 and 360 <= h <= 2160 and w % 2 == 0 and h % 2 == 0, 'size must be even, 640x360 .. 3840x2160'
    index = json.loads((a.romfs / 'index.json').read_text())
    entry = next(r for r in index['files'] if r['path'] == NAME)
    with (a.romfs / 'romfs.bin').open('rb') as f:
        f.seek(entry['offset']); original = f.read(entry['size'])
    sarc = bytearray(yaz0_decode(original))
    off, size = sarc_member(bytes(sarc), MEMBER)
    table, changes = scaled_table(bytes(sarc[off:off + size]), w, h)
    sarc[off:off + size] = table
    patched = yaz0_store(bytes(sarc))
    assert yaz0_decode(patched) == bytes(sarc), 'Yaz0 round trip failed'
    out = a.out if a.flat else a.out / f'{w}x{h}'
    (out / Path(NAME).parent).mkdir(parents=True, exist_ok=True)
    (out / NAME).write_bytes(patched)
    sha = lambda d: hashlib.sha256(d).hexdigest()
    (out / 'manifest.tsv').write_text(f'{NAME}\t{len(patched)}\t{sha(patched)}\t{sha(original)}\n')
    # The runtime takes the window, swapchain and HUD size from here when TOTK_RENDER_RES is unset (game_patches.c), so the
    # code sizes and this table cannot disagree.
    (out / 'render-res.txt').write_text(f'{w}x{h}\n')
    for name, rank, old, new in changes:
        print(f'{name:9s} rank {rank}: {old[0]}x{old[1]} -> {new[0]}x{new[1]}')
    print(f'wrote {out / NAME} ({len(patched)} bytes) and {out / "manifest.tsv"}')


if __name__ == '__main__':
    main()
