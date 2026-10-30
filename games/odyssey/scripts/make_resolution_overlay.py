#!/usr/bin/env python3
"""Odyssey 1.3.0: a RomFS overlay that raises the docked scene render size (RESOLUTION_RESEARCH.md, option A data part).

    .venv/bin/python scripts/make_resolution_overlay.py 1920x1080 [--work Odyssey_build] [--output <dir>]

The scene size is data: SystemData/ProjectGraphicsQuality.szs (Yaz0 SARC) -> ProjectGraphicsQuality.byml ->
ProjectGraphicsQualityParamArray. Entry "Console" (ParamType 0, Rank 0) is 1600x900; "SnapShot" (photo mode) too; the
Console dynamic-resolution ranks 1-5 step down from it. This rewrites ScreenWidthScale / ScreenHeightScale of those
entries in place (same node types, no other byte changes, checked), scaling the ranks by the same factor, and writes
<output>/SystemData/ProjectGraphicsQuality.szs plus manifest.tsv for runtime/romfs.c (TOTK_ROMFS_PATCHES=<output>).
The SZS is re-packed as Yaz0 with literal runs only (valid Yaz0; a little larger). Game data stays in the ignored work
folder; this prints only sizes, hashes and the values changed.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

GAME = Path(__file__).resolve().parents[1]
PATH = 'SystemData/ProjectGraphicsQuality.szs'


def yaz0_decompress(d):
    assert d[:4] == b'Yaz0', 'not Yaz0'
    size = struct.unpack_from('>I', d, 4)[0]; out = bytearray(); i = 16
    while len(out) < size:
        flags = d[i]; i += 1
        for bit in range(7, -1, -1):
            if len(out) >= size: break
            if flags >> bit & 1: out.append(d[i]); i += 1
            else:
                b1, b2 = d[i], d[i + 1]; i += 2; dist = ((b1 & 0xf) << 8 | b2) + 1; n = b1 >> 4
                if n == 0: n = d[i] + 0x12; i += 1
                else: n += 2
                for _ in range(n): out.append(out[-dist])
    return bytes(out)


def yaz0_literal(data):
    out = bytearray(b'Yaz0' + struct.pack('>I', len(data)) + bytes(8))
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]; out.append(0xff if len(chunk) == 8 else (0xff << (8 - len(chunk))) & 0xff); out += chunk
    return bytes(out)


def sarc_members(data):
    assert data[:4] == b'SARC'
    bom = data[6:8]; e = '<' if bom == b'\xff\xfe' else '>'
    header = struct.unpack_from(e + 'H', data, 4)[0]; start = struct.unpack_from(e + 'I', data, 12)[0]
    count = struct.unpack_from(e + 'H', data, header + 6)[0]; names = header + 12 + count * 16 + 8
    out = {}
    for k in range(count):
        _, attribute, lo, hi = struct.unpack_from(e + '4I', data, header + 12 + k * 16)
        off = names + (attribute & 0xffffff) * 4
        out[data[off:data.index(0, off, start)].decode()] = (start + lo, start + hi)
    return out


class Byml:
    """In-place access to a little-endian BYML (v2/v3): hash 0xC1 entries {u24 key, u8 type, u32 value}, array 0xC0."""
    def __init__(self, b):
        assert b[:2] == b'YB', 'not a little-endian BYML'
        self.b = b; key_table, _, self.root = struct.unpack_from('<III', b, 4); self.keys = self._strings(key_table)
    def _strings(self, off):
        n = struct.unpack_from('<I', self.b, off)[0] >> 8; offs = struct.unpack_from(f'<{n + 1}I', self.b, off + 4)
        return [self.b[off + o:self.b.index(0, off + o)].decode() for o in offs[:n]]
    def hash_entries(self, node):
        assert self.b[node] == 0xc1, hex(self.b[node])
        for k in range(struct.unpack_from('<I', self.b, node)[0] >> 8):
            e = node + 4 + 8 * k
            yield self.keys[struct.unpack_from('<I', self.b, e)[0] & 0xffffff], self.b[e + 3], e + 4
    def field(self, node, name):
        for key, t, at in self.hash_entries(node):
            if key == name: return t, at
        raise KeyError(name)
    def array(self, node):
        assert self.b[node] == 0xc0, hex(self.b[node])
        n = struct.unpack_from('<I', self.b, node)[0] >> 8; types = self.b[node + 4:node + 4 + n]
        base = node + 4 + ((n + 3) & ~3)
        return [(types[k], base + 4 * k) for k in range(n)]
    def value(self, t, at):
        if t == 0xd1: return struct.unpack_from('<i', self.b, at)[0]
        if t == 0xd2: return struct.unpack_from('<f', self.b, at)[0]
        if t == 0xd3: return struct.unpack_from('<I', self.b, at)[0]
        return struct.unpack_from('<I', self.b, at)[0]
    def set(self, t, at, v):
        if t == 0xd1: struct.pack_into('<i', self.b, at, int(round(v)))
        elif t == 0xd2: struct.pack_into('<f', self.b, at, float(v))
        elif t == 0xd3: struct.pack_into('<I', self.b, at, int(round(v)))
        else: raise TypeError('unexpected node type %#x' % t)
    def string(self, t, at):   # a string value indexes the value string table
        _, strings_off, _ = struct.unpack_from('<III', self.b, 4)
        return self._strings(strings_off)[struct.unpack_from('<I', self.b, at)[0]] if t == 0xa0 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('size'); ap.add_argument('--work', type=Path, default=GAME / 'Odyssey_build'); ap.add_argument('--output', type=Path)
    a = ap.parse_args()
    w, h = (int(x) for x in a.size.lower().split('x'))
    assert 1600 <= w <= 3840 and 900 <= h <= 2160 and abs(w / h - 16 / 9) < 0.01, 'a 16:9 size from 1600x900 to 3840x2160'
    ext = a.work.resolve() / 'extracted/local/romfs'
    index = json.loads((ext / 'index.json').read_text()); files = {f['path']: f for f in index['files']}
    f = files[PATH]
    with (ext / 'romfs.bin').open('rb') as img: img.seek(f['offset']); raw = img.read(f['size'])
    assert len(raw) == f['size']
    sarc = bytearray(yaz0_decompress(raw))
    members = sarc_members(sarc)
    name = next(n for n in members if n.endswith('.byml')); lo, hi = members[name]
    by = Byml(bytearray(sarc[lo:hi]))
    _, arr_at = by.field(by.root, 'ProjectGraphicsQualityParamArray')
    arr = struct.unpack_from('<I', by.b, arr_at)[0]
    changed = []
    for t, at in by.array(arr):
        node = struct.unpack_from('<I', by.b, at)[0]
        fields = {k: (tt, aa) for k, tt, aa in by.hash_entries(node)}
        nm = by.string(*fields['Name']) if 'Name' in fields else None
        if nm not in ('Console', 'SnapShot'): continue
        pt, rank = by.value(*fields['ParamType']), by.value(*fields['Rank'])
        (tw, aw), (th, ah) = fields['ScreenWidthScale'], fields['ScreenHeightScale']
        ow, oh = by.value(tw, aw), by.value(th, ah)
        if nm == 'SnapShot' or rank == 0: nw, nh = w, h
        else: nw, nh = ow * w / 1600, oh * h / 900   # the dynamic-resolution steps keep their ratio to the base
        if (ow, oh) != (nw, nh):
            by.set(tw, aw, nw); by.set(th, ah, nh); changed.append(dict(name=nm, param_type=pt, rank=rank, was=[ow, oh], now=[by.value(tw, aw), by.value(th, ah)]))
    assert any(c['name'] == 'Console' and c['rank'] == 0 for c in changed), 'Console base entry not found / unchanged'
    old = bytes(sarc[lo:hi]); diff = [k for k in range(len(old)) if old[k] != by.b[k]]
    assert len(old) == len(by.b) and len(diff) <= 8 * len(changed), 'only the size fields may change'
    sarc[lo:hi] = by.b
    data = yaz0_literal(bytes(sarc)); assert yaz0_decompress(data) == bytes(sarc)
    out = (a.output or a.work.resolve() / f'romfs-res/{w}x{h}').resolve()
    (out / PATH).parent.mkdir(parents=True, exist_ok=True); (out / PATH).write_bytes(data)
    line = '\t'.join([PATH, str(len(data)), hashlib.sha256(data).hexdigest(), hashlib.sha256(raw).hexdigest()])
    (out / 'manifest.tsv').write_text(f'# path\tsize\tsha256\toriginal_sha256 (scripts/make_resolution_overlay.py {w}x{h})\n{line}\n')
    print(json.dumps(dict(status='pass', size=f'{w}x{h}', member=name, changed=changed, changed_bytes=len(diff), dir=str(out)), indent=1))


if __name__ == '__main__':
    sys.exit(main())
