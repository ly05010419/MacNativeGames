#!/usr/bin/env python3
"""Fit a texture pack's size tables to the game version the app is built from (a pack made for an older version, such as
the HDPacks 4K pack for 1.0.0-1.2.1, played on 1.4.3).

A texture pack replaces textures with bigger ones, so it also ships the two tables the game sizes its memory by, made
from the version it was built for:
- System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs, the heap sizes. Another version's table would also set every heap
  the pack did not mean to change to that version's size, so this writes the version's own table with only the heaps the
  pack makes bigger raised to the pack's size, in place over the pack's file.
- System/Resource/ResourceSizeTable.Product.<n>.Nin_NX_NVN.rsizetable.zs (a RESTBL, entries keyed by the CRC32 of the
  path or by the path), how much memory each file needs. The game reads only its own version's <n>, which an older pack
  does not have, and stops loading a file bigger than its entry. This writes the version's table (the mod folder's own
  copy if it has one, else the original) with the entry of every replaced file grown by as much as the file grew. Only
  files stored uncompressed (not .zs) are counted: their size is what the entry measures. Texture packs replace .txtg
  files.

Run it on the mod folder before scripts/make_mod_manifest.py, and again whenever the pack's files change. Nothing of the
pack or the game is in this repository: the tables are written from the player's own RomFS and pack.

usage: texture_pack_tables.py <mod folder> (--app <Tears of the Kingdom.app> | --romfs <folder>) [--version v143]
Needs Python 3.14 (compression.zstd).
"""
import argparse
from pathlib import Path
import struct
import sys
import zlib

try:
    from compression import zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd)')

from make_mod_manifest import VERSIONS, read_index

HEAPDEF = 'System/HeapSize/Product.Nin_NX_NVN.HeapDef.byml.zs'


def heap_size_fields(byml):
    """HeapDef BYML (v7, "YB") -> {heap name: offset of its Size field (signed 32-bit, KiB)}."""
    assert byml[:2] == b'YB' and struct.unpack_from('<H', byml, 2)[0] == 7, 'not a BYML v7 heap table'
    keys_at, _, root = struct.unpack_from('<III', byml, 4)
    count = struct.unpack_from('<I', byml, keys_at)[0] >> 8
    offsets = struct.unpack_from(f'<{count + 1}I', byml, keys_at + 4)
    keys = [byml[keys_at + o: byml.index(0, keys_at + o)].decode() for o in offsets[:count]]
    def entries(node):
        assert byml[node] == 0xc1, 'not a hash node'
        for i in range(struct.unpack_from('<I', byml, node)[0] >> 8):
            at = node + 4 + 8*i
            yield keys[struct.unpack_from('<I', byml, at)[0] & 0xffffff], byml[at + 3], at + 4
    heaps = next(struct.unpack_from('<I', byml, at)[0] for name, kind, at in entries(root) if name == 'Heaps')
    fields = {}
    for name, kind, at in entries(heaps):
        if kind == 0xc1:
            for field, fkind, fat in entries(struct.unpack_from('<I', byml, at)[0]):
                if field == 'Size' and fkind == 0xd1:
                    fields[name] = fat
    return fields


def fit_heapdef(original, pack):
    """The `original` table (decompressed) with each heap the `pack` table makes bigger raised to the pack's size, and
    the list of (heap, original KiB, pack KiB) raised."""
    out = bytearray(original)
    mine, theirs = heap_size_fields(original), heap_size_fields(pack)
    raised = []
    for name, at in mine.items():
        if name in theirs:
            old, new = struct.unpack_from('<i', original, at)[0], struct.unpack_from('<i', pack, theirs[name])[0]
            if new > old:
                struct.pack_into('<i', out, at, new)
                raised.append((name, old, new))
    return bytes(out), raised


def restbl_entries(table):
    """RESTBL -> ({CRC32: offset of its size}, {path: offset of its size})."""
    assert table[:6] == b'RESTBL', 'not a RESTBL'
    name_len, crc_count, name_count = struct.unpack_from('<III', table, 10)
    crcs = {struct.unpack_from('<I', table, 22 + i*8)[0]: 22 + i*8 + 4 for i in range(crc_count)}
    base = 22 + crc_count*8
    names = {}
    for i in range(name_count):
        at = base + i*(name_len + 4)
        names[table[at:table.index(0, at)].decode()] = at + name_len
    return crcs, names


def grow_restbl(table, original, growth):
    """`table` with the entry of each path in `growth` (RomFS path -> bytes) at least its entry in the `original` table
    plus that growth; returns the table, the number of entries grown and the paths no entry names."""
    out = bytearray(table)
    (ocrc, oname), (tcrc, tname) = restbl_entries(original), restbl_entries(table)
    grown, missing = 0, []
    for path, extra in sorted(growth.items()):
        key = path.removesuffix('.zs')
        if key in oname and key in tname:
            src, dst = oname[key], tname[key]
        elif zlib.crc32(key.encode()) in ocrc and zlib.crc32(key.encode()) in tcrc:
            h = zlib.crc32(key.encode()); src, dst = ocrc[h], tcrc[h]
        else:
            missing.append(path); continue
        need = struct.unpack_from('<I', original, src)[0] + extra
        if need > struct.unpack_from('<I', out, dst)[0]:
            struct.pack_into('<I', out, dst, need); grown += 1
    return bytes(out), grown, missing


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder, files under their RomFS paths (the pack\'s romfs folder merged in)')
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path, help='the built Tears of the Kingdom.app')
    source.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and runtime-index.bin')
    parser.add_argument('--version', choices=VERSIONS, help='the game version (default: the mod folder name)')
    args = parser.parse_args()
    version = args.version or (args.mod.resolve().name if args.mod.resolve().name in VERSIONS else None)
    romfs = args.romfs
    if args.app:
        if not version:
            parser.error('name the game version with --version (or name the mod folder v0, v112, v121 or v143)')
        romfs = args.app/f'Contents/Resources/totk/extracted/{version}/romfs'
    for need in ('romfs.bin', 'runtime-index.bin'):
        if not (romfs/need).is_file():
            parser.error(f'{romfs/need} not found')
    if not args.mod.is_dir():
        parser.error(f'{args.mod} is not a folder')
    index = read_index(romfs/'runtime-index.bin')
    with (romfs/'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); return image.read(size)
        heapdef_raw = read(HEAPDEF)
        restbl = next(p for p in index if p.startswith('System/Resource/ResourceSizeTable.') and p.endswith('.rsizetable.zs'))
        restbl_raw = read(restbl)

    pack_heapdef = args.mod/HEAPDEF
    if pack_heapdef.is_file():
        table, raised = fit_heapdef(zstd.decompress(heapdef_raw), zstd.decompress(pack_heapdef.read_bytes()))
        data = zstd.compress(table, level=19)
        assert zstd.decompress(data) == table
        pack_heapdef.write_bytes(data)
        print(f'{HEAPDEF}: this version\'s table, ' + (', '.join(f'{n} {o} -> {v} KiB' for n, o, v in raised) or 'no heap raised'))
    else:
        print(f'{HEAPDEF}: not in the mod folder, left alone')

    growth = {}
    for file in args.mod.rglob('*'):
        path = file.relative_to(args.mod).as_posix()
        if file.is_file() and not path.endswith('.zs') and path in index and file.stat().st_size > index[path][1]:
            growth[path] = file.stat().st_size - index[path][1]
    target = args.mod/restbl
    original = zstd.decompress(restbl_raw)
    table, grown, missing = grow_restbl(zstd.decompress(target.read_bytes()) if target.is_file() else original, original, growth)
    data = zstd.compress(table, level=19)
    assert zstd.decompress(data) == table
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f'{restbl}: {grown} entries grown for {len(growth)} bigger files' +
          (f', {len(missing)} without an entry (for example {missing[0]})' if missing else ''))


if __name__ == '__main__':
    main()
