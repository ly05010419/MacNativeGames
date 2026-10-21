#!/usr/bin/env python3
"""List Breath of the Wild location markers (shrines, towers, stables, ...) with their world position and in-game name,
for survey move (runtime/world_survey.c) and for test routes.
usage: location_markers.py <work>/extracted/botw160/romfs [--lang CNzh] [filter text]
Reads Pack/Bootup.pack -> Map/MainField/Static.smubin (Yaz0 BYML) and Pack/Bootup_<lang>.pack -> Msg_<lang>.product.ssarc ->
StaticMsg/LocationMarker.msbt. No game data is written anywhere."""
import json, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'totk' / 'scripts'))   # the shared Yaz0 and SARC readers
from prepare_shader_seed import yaz0            # Yaz0 decoder the seed tooling already has
from make_romfs_patches import sarc_members     # SARC member table

def romfs_file(romfs, path):
    index = json.loads((romfs / 'index.json').read_text())
    f = next(x for x in index['files'] if x['path'] == path)
    with (romfs / 'romfs.bin').open('rb') as img:
        img.seek(f['offset']); return img.read(f['size'])

def unyaz(b): return yaz0(b) if b[:4] == b'Yaz0' else b

def byml(data):
    """Minimal little-endian BYML v2+ reader (Switch): dict/array/string/int/float/bool/int64/uint/uint64."""
    assert data[:2] == b'YB', data[:4]
    hash_off, str_off, root_off = struct.unpack_from('<III', data, 4)
    def strtab(off):
        if not off: return []
        assert data[off] == 0xc2
        n = struct.unpack_from('<I', data, off)[0] >> 8
        offs = struct.unpack_from('<%dI' % (n + 1), data, off + 4)
        return [data[off + offs[i]:off + offs[i + 1] - 1].decode('utf-8', 'replace') for i in range(n)]
    hashes, strings = strtab(hash_off), strtab(str_off)
    def node(t, v, pos):
        if t == 0xa0: return strings[v]
        if t == 0xd0: return bool(v)
        if t == 0xd1: return struct.unpack('<i', struct.pack('<I', v))[0]
        if t == 0xd2: return struct.unpack('<f', struct.pack('<I', v))[0]
        if t == 0xd3: return v
        if t == 0xd4: return struct.unpack_from('<q', data, v)[0]
        if t == 0xd5: return struct.unpack_from('<Q', data, v)[0]
        if t == 0xd6: return struct.unpack_from('<d', data, v)[0]
        if t in (0xc0, 0xc1): return container(v)
        if t == 0xff: return None
        raise ValueError('byml node type %#x at %#x' % (t, pos))
    def container(off):
        t = data[off]; n = struct.unpack_from('<I', data, off)[0] >> 8
        if t == 0xc0:
            types = data[off + 4:off + 4 + n]; vals = off + 4 + ((n + 3) & ~3)
            return [node(types[i], struct.unpack_from('<I', data, vals + i * 4)[0], vals + i * 4) for i in range(n)]
        if t == 0xc1:
            out = {}
            for i in range(n):
                e = off + 4 + i * 8; key = struct.unpack_from('<I', data, e)[0] & 0xffffff; ty = data[e + 3]
                out[hashes[key]] = node(ty, struct.unpack_from('<I', data, e + 4)[0], e + 4)
            return out
        raise ValueError('byml root type %#x' % t)
    return container(root_off)

def msbt_labels(data):
    """label -> text (first attribute-free text of each entry) for an MSBT (little-endian on Switch)."""
    assert data[:8] == b'MsgStdBn'
    bom = data[8:10]; e = '<' if bom == b'\xff\xfe' else '>'
    nsec = struct.unpack_from(e + 'H', data, 0x0e)[0]; pos = 0x20; sections = {}
    for _ in range(nsec):
        name = data[pos:pos + 4]; size = struct.unpack_from(e + 'I', data, pos + 4)[0]
        sections[name] = data[pos + 0x10:pos + 0x10 + size]; pos = pos + 0x10 + ((size + 15) & ~15)
    lbl, txt = sections[b'LBL1'], sections[b'TXT2']
    groups = struct.unpack_from(e + 'I', lbl, 0)[0]; labels = {}
    for g in range(groups):
        cnt, off = struct.unpack_from(e + 'II', lbl, 4 + g * 8)
        for _ in range(cnt):
            ln = lbl[off]; name = lbl[off + 1:off + 1 + ln].decode(); idx = struct.unpack_from(e + 'I', lbl, off + 1 + ln)[0]
            labels[idx] = name; off += 1 + ln + 4
    n = struct.unpack_from(e + 'I', txt, 0)[0]; offs = struct.unpack_from(e + '%dI' % n, txt, 4)
    texts = {}
    for i in range(n):
        end = offs[i + 1] if i + 1 < n else len(txt)
        raw = txt[offs[i]:end]
        s = raw.decode('utf-16-le' if e == '<' else 'utf-16-be', 'replace')
        texts[labels.get(i, str(i))] = s.split('\x00')[0]
    return texts

def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    romfs = Path(sys.argv[1]); lang = 'CNzh'; args = sys.argv[2:]
    if args[:1] == ['--lang']: lang = args[1]; args = args[2:]
    needle = args[0] if args else ''
    title = romfs_file(romfs, 'Pack/Bootup.pack'); m = sarc_members(title)
    lo, hi = m['Map/MainField/Static.smubin']; static = byml(unyaz(title[lo:hi]))
    boot = romfs_file(romfs, 'Pack/Bootup_%s.pack' % lang); bm = sarc_members(boot)
    lo, hi = bm['Message/Msg_%s.product.ssarc' % lang]; msg = unyaz(boot[lo:hi]); mm = sarc_members(msg)
    lo, hi = mm['StaticMsg/LocationMarker.msbt']; names = msbt_labels(msg[lo:hi])
    rows = []
    for e in static.get('LocationMarker', []):
        mid = e.get('MessageID') or ''; t = e.get('Translate') or {}
        name = names.get(mid, '')
        rows.append((mid, name, t.get('X'), t.get('Y'), t.get('Z'), e.get('SaveFlag', ''), e.get('Icon', '')))
    for r in rows:
        line = '%-24s %-16s %10.2f %10.2f %10.2f  %s %s' % (r[0], r[1], r[2] or 0, r[3] or 0, r[4] or 0, r[5], r[6])
        if not needle or needle in line: print(line)
    print('%d markers' % len(rows), file=sys.stderr)
if __name__ == '__main__': main()
