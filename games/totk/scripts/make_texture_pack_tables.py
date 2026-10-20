#!/usr/bin/env python3
"""Make the two System files the HDPacks.com 4K texture pack needs on 1.4.3 (the pack ships them for 1.0.0 - 1.2.1, its
v1.0.1 patch for 1.4.2). The textures themselves (RomFS/TexToGo/*.txtg) are the same bytes in 1.0.0 and 1.4.3, so they are
used as they are; only these tables name the version.

  System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs
      The game's own table with the size of each of the pack's textures replaced by the size the pack gives it (a RESTBL
      entry is crc32(path) -> size; the pack's 1.0.0-1.2.1 and 1.4.2 tables agree on all but a few; the newest wins). Entries of other
      files, and the 1.4.3 additions, stay as the game ships them.
  System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs
      The game's own HeapDef with the heaps the pack enlarges (TextureHeap, ゲームグラフィックス, 草) set to the pack's sizes.
      Every other heap must already be equal in the game's and the pack's file, or the script stops.

Input: the pack's System folder (extract it from the pack's rar; the v1.0.1 patch's is the newest) and the IVFC-verified
RomFS image (extracted/<version>/romfs). Output: <output>/System/..., ready to copy into the mods folder with the
textures; run make_mod_manifest.py on the folder afterwards.
usage: make_texture_pack_tables.py <pack>/4kTextures/RomFS/System <patch>/4kTextures/RomFS/System <pack>/4kTextures/RomFS/TexToGo --output <mod folder>
"""
import argparse, json, struct, zlib
from pathlib import Path
import compression.zstd as zstd

root = Path(__file__).resolve().parents[1]
TABLE = 'System/Resource/ResourceSizeTable.Product.143.Nin_NX_NVN.rsizetable.zs'
HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'
GROWN = ('TextureHeap', 'ゲームグラフィックス', '草')

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('system', type=Path, nargs='+', help="the pack's RomFS/System folder, then the patch's (its HeapDef is the one used; every table is read)")
parser.add_argument('textures', type=Path, help="the pack's RomFS/TexToGo folder (the paths whose sizes are taken over)")
parser.add_argument('--output', type=Path, required=True, help='mod folder to write System/ into')
parser.add_argument('--heapdef-overlay', type=Path, help='write the HeapDef (with a manifest.tsv) into this RomFS overlay folder (extracted/<version>/romfs-patches-out/4k, '
                    'what TOTK_4K=1 loads) instead of the mod folder: a mod and the build must not both replace it')
parser.add_argument('--version', choices=('v143',), default='v143')
args = parser.parse_args()

manifest = json.loads((root / f'extracted/{args.version}/romfs/index.json').read_text())
assert manifest['status'] == 'pass'
files = {f['path']: f for f in manifest['files']}
with (root / f'extracted/{args.version}/romfs/romfs.bin').open('rb') as image:
    def read(path): f = files[path]; image.seek(f['offset']); b = image.read(f['size']); assert len(b) == f['size']; return b
    game_heapdef_raw = read(HEAPDEF)
    game_table, game_heapdef = zstd.decompress(read(TABLE)), zstd.decompress(game_heapdef_raw)

# RESTBL: "RESTBL", u32 version, u32 name length, u32 hash count, u32 collision count, then (crc32, size) pairs sorted by crc32
# and the collision records (name[name length], size). Version 0 (the pack's tables) has no collisions.
def restbl(data):
    assert data[:6] == b'RESTBL'
    version, name_length, count, collisions = struct.unpack_from('<IIII', data, 6)
    assert len(data) == 22 + count * 8 + collisions * (name_length + 4), 'unexpected RESTBL layout'
    return dict(struct.unpack_from('<II', data, 22 + i * 8) for i in range(count)), collisions

paths = sorted('TexToGo/' + p.name for p in args.textures.glob('*.txtg'))
assert paths, 'no .txtg files in ' + str(args.textures)
game, collisions = restbl(game_table)
pack_tables = sorted(t for s in args.system for t in s.glob('Resource/ResourceSizeTable.Product.*.rsizetable.zs'))
assert pack_tables, 'no pack tables under ' + str(args.system[0] / 'Resource')
sizes, disagree = {}, []
for table in sorted(pack_tables, key=lambda t: t.name.split('.')[2]):   # oldest first: the newest table has the last word
    entries, _ = restbl(zstd.decompress(table.read_bytes()))
    for p in paths:
        h = zlib.crc32(p.encode())
        if h not in entries: raise SystemExit(f'{table.name}: no entry for {p}')
        if sizes.get(p, entries[h]) != entries[h]: disagree.append((p, sizes[p], entries[h], table.name))
        sizes[p] = entries[h]
for p, old, new, name in disagree: print(f'{p}: {old} -> {new} ({name})')
patched, absent = bytearray(game_table), []
index = {struct.unpack_from('<I', game_table, 22 + i * 8)[0]: 22 + i * 8 for i in range(len(game))}
for p in paths:
    at = index.get(zlib.crc32(p.encode()))
    if at is None: absent.append(p); continue   # a texture 1.4.3 has no table entry for: the game does not size it either
    struct.pack_into('<I', patched, at + 4, sizes[p])
changed = sum(1 for p in paths if p not in absent and game[zlib.crc32(p.encode())] != sizes[p])

# BYML v7 hash node 0xC1 {u24 key index, u8 type, u32 value}; Heaps -> heap name -> Size (int, 0xD1, stored inline).
def heaps(byml):
    assert byml[:2] == b'YB' and struct.unpack_from('<H', byml, 2)[0] == 7
    key_table, _, root_node = struct.unpack_from('<III', byml, 4)
    n = struct.unpack_from('<I', byml, key_table)[0] >> 8
    offs = struct.unpack_from(f'<{n + 1}I', byml, key_table + 4)
    keys = [byml[key_table + o: byml.index(0, key_table + o)].decode() for o in offs[:n]]
    def node(off):
        assert byml[off] == 0xc1
        return {keys[struct.unpack_from('<I', byml, off + 4 + 8 * i)[0] & 0xffffff]:
                (byml[off + 7 + 8 * i], off + 8 + 8 * i) for i in range(struct.unpack_from('<I', byml, off)[0] >> 8)}
    out = {}
    for name, (_, at) in node(struct.unpack_from('<I', byml, node(root_node)['Heaps'][1])[0]).items():
        out[name] = {k: (t, a) for k, (t, a) in node(struct.unpack_from('<I', byml, at)[0]).items()}
    return out
pack_heapdef = zstd.decompress(next(args.system[-1].glob('HeapSize/*.byml.zs')).read_bytes())
ours, theirs = heaps(game_heapdef), heaps(pack_heapdef)
patched_heapdef = bytearray(game_heapdef)
for name, fields in ours.items():
    if name not in theirs: continue
    for key, (_, at) in fields.items():
        have, want = struct.unpack_from('<i', game_heapdef, at)[0], struct.unpack_from('<i', pack_heapdef, theirs[name][key][1])[0]
        if have == want: continue
        if name not in GROWN or key != 'Size': raise SystemExit(f'HeapDef {name}.{key}: the game has {have}, the pack {want} (not a heap the pack grows)')
        struct.pack_into('<i', patched_heapdef, at, want)
        print(f'HeapDef {name}: {have} -> {want} KiB')

import hashlib
def write(target, data):
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(zstd.compress(data, level=19))
    assert zstd.decompress(target.read_bytes()) == data
write(args.output / TABLE, bytes(patched))
if args.heapdef_overlay:
    write(args.heapdef_overlay / HEAPDEF, bytes(patched_heapdef))
    out = (args.heapdef_overlay / HEAPDEF).read_bytes()
    (args.heapdef_overlay / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_texture_pack_tables.py)\n' + '\t'.join(
        [HEAPDEF, str(len(out)), hashlib.sha256(out).hexdigest(), hashlib.sha256(game_heapdef_raw).hexdigest()]) + '\n')
else:
    write(args.output / HEAPDEF, bytes(patched_heapdef))
print(json.dumps(dict(status='pass', textures=len(paths), sizes_changed=changed, without_table_entry=absent, output=str(args.output))))
