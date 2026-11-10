#!/usr/bin/env python3
"""bench_locations.py <romfs dir> [--out locations.json]: the game's named map locations (LocationMarker actors of the
surface and sky (Banc/MainField) and the Depths (Banc/MinusField)), with their positions, read from the player's own
RomFS (romfs.bin + index.json, as build_app.py extracts it). The benchmark route (bench_route.py) is built from these.
Nothing from the game is kept in the repository: the output is written to the work folder."""
import argparse
import json
import struct
import sys
from pathlib import Path

import zstandard


class RomFS:
    def __init__(self, root):
        self.root = Path(root)
        self.blob = open(self.root / 'romfs.bin', 'rb')
        self.files = {f['path']: f for f in json.loads((self.root / 'index.json').read_text())['files']}
        self.dicts = {}
        pack = self.read_raw('Pack/ZsDic.pack.zs')
        data = zstandard.ZstdDecompressor().decompress(pack, max_output_size=1 << 28)
        for name, blob in sarc(data).items():
            if name.endswith('.zsdic'):
                d = zstandard.ZstdCompressionDict(blob)
                self.dicts[d.dict_id()] = d

    def read_raw(self, path):
        f = self.files[path]
        self.blob.seek(f['offset'])
        return self.blob.read(f['size'])

    def read(self, path):
        raw = self.read_raw(path)
        if not path.endswith('.zs'):
            return raw
        dict_id = zstandard.get_frame_parameters(raw).dict_id
        if dict_id:
            return zstandard.ZstdDecompressor(dict_data=self.dicts[dict_id]).decompress(raw, max_output_size=1 << 28)
        return zstandard.ZstdDecompressor().decompress(raw, max_output_size=1 << 28)


def sarc(d):
    """SARC: header 0x14 (u32 data offset at +0xc), SFAT nodes {hash, attr (name offset / 4 when bit 24), start, end},
    SFNT names after the nodes."""
    data = struct.unpack_from('<I', d, 0xC)[0]
    count = struct.unpack_from('<H', d, 0x1A)[0]
    nodes = 0x14 + 0xC
    names = nodes + count * 16 + 8
    out = {}
    for i in range(count):
        _, attr, start, end = struct.unpack_from('<IIII', d, nodes + i * 16)
        name = ''
        if attr & 0x01000000:
            o = names + (attr & 0xFFFF) * 4
            name = d[o:d.index(b'\0', o)].decode()
        out[name] = d[data + start:data + end]
    return out


def byml(d):
    """BYML v2-v7 (little-endian 'YB'): dictionaries, arrays, strings and scalars as Python values."""
    if d[:2] != b'YB':
        raise ValueError('not a little-endian BYML')
    keys_off, strs_off, root_off = struct.unpack_from('<III', d, 4)

    def table(off):
        if not off:
            return []
        n = struct.unpack_from('<I', d, off)[0] >> 8
        offs = struct.unpack_from(f'<{n}I', d, off + 4)
        return [d[off + o:d.index(b'\0', off + o)].decode('utf-8', 'replace') for o in offs]

    keys, strs = table(keys_off), table(strs_off)

    def value(kind, raw):
        if kind == 0xA0: return strs[raw]
        if kind == 0xD0: return bool(raw)
        if kind == 0xD1: return struct.unpack('<i', struct.pack('<I', raw))[0]
        if kind == 0xD2: return struct.unpack('<f', struct.pack('<I', raw))[0]
        if kind == 0xD3: return raw
        if kind in (0xD4, 0xD5, 0xD6):
            v = struct.unpack_from('<Q', d, raw)[0]
            return struct.unpack('<q' if kind == 0xD4 else '<d', struct.pack('<Q', v))[0] if kind != 0xD5 else v
        if kind in (0xC0, 0xC1): return node(raw)
        if kind == 0xFF: return None
        return raw

    def node(off):
        kind = d[off]
        n = struct.unpack_from('<I', d, off)[0] >> 8
        if kind == 0xC0:
            types = d[off + 4:off + 4 + n]
            vals = off + 4 + ((n + 3) & ~3)
            return [value(types[i], struct.unpack_from('<I', d, vals + i * 4)[0]) for i in range(n)]
        if kind == 0xC1:
            out = {}
            for i in range(n):
                e = off + 4 + i * 8
                k = int.from_bytes(d[e:e + 3], 'little')
                out[keys[k]] = value(d[e + 3], struct.unpack_from('<I', d, e + 4)[0])
            return out
        raise ValueError(f'BYML node {kind:#x}')

    return node(root_off) if root_off else None


def locations(rom):
    out = []
    for path in sorted(rom.files):
        field = 'surface' if path.startswith('Banc/MainField/') else 'depths' if path.startswith('Banc/MinusField/') else None
        if not field or not path.endswith('.bcett.byml.zs'):
            continue
        try:
            doc = byml(rom.read(path))
        except Exception as e:   # one damaged file does not stop the route
            print(f'skipped {path}: {e}', file=sys.stderr)
            continue
        for actor in (doc or {}).get('Actors', []):
            if not isinstance(actor, dict) or actor.get('Gyaml') != 'LocationMarker':
                continue
            t = actor.get('Translate') or []
            dyn = actor.get('Dynamic') or {}
            name = dyn.get('Location') or ''
            if len(t) == 3:
                out.append(dict(name=name, field=field, x=t[0], y=t[1], z=t[2], file=path.split('/')[-1]))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('romfs', type=Path)
    p.add_argument('--out', type=Path)
    a = p.parse_args()
    locs = locations(RomFS(a.romfs))
    text = json.dumps(locs, indent=1, ensure_ascii=False)
    if a.out:
        a.out.write_text(text + '\n')
    print(f'{len(locs)} locations ({sum(l["field"] == "surface" for l in locs)} surface/sky, '
          f'{sum(l["field"] == "depths" for l in locs)} depths)', file=sys.stderr)
    if not a.out:
        print(text)


if __name__ == '__main__':
    main()
