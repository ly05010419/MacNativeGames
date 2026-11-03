#!/usr/bin/env python3
"""Write the manifest.tsv of a player's RomFS mod for Pokémon Violet (runtime/romfs.c loads it from PSV_ROMFS_MODS, which
the launcher sets to each chosen mods/<name>/romfs next to the work folder; `make mods` runs this for every one).

The mod folder holds the replacement files under their RomFS paths. Each line of the manifest names a path, the
replacement's size and SHA-256 and the original's SHA-256; the game checks all of them at start, so a mod made for
another game version stops the start instead of loading. A file the RomFS lacks is added (its original's SHA-256 is
"-"), unless a RomFS path differs from it only in case: that is a misspelt replacement (paths are case-sensitive), so
nothing is written. The originals come from the RomFS the game was prepared with, which stays in the work folder (the app
holds only the launcher): <work>/extracted/<version>/romfs (romfs.bin and runtime-index.bin).

Trinity mods (loose files the game keeps inside arc/data.trpfs, as Trinity Mod Loader makes them): the game looks a
file up by the FNV-1a hash of its path in the sorted hash list of arc/data.trpfd and reads the loose RomFS file when the
hash is not there. For every added file whose hash the index lists, this script writes arc/data.trpfd into the mod
folder with that hash moved by one (the list stays sorted and keeps its size), and the manifest replaces the index with
it. A data.trpfd the mod brings itself (Trinity Mod Loader's output) is used as it is; a generated one is marked by
arc/.data.trpfd-generated and made again from the original each time.

usage: make_mod_manifest.py <mod folder> --romfs <work>/extracted/<version>/romfs
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

CHUNK = 8 << 20


def read_index(path):
    """RomFS path -> (offset, size) from runtime-index.bin (scripts/prepare_romfs_backend.py writes it)."""
    data = path.read_bytes()
    magic, count, strings = struct.unpack_from('<8sQQ', data, 0)
    if magic != b'PSVRFS01' or strings != 24 + count*24 or strings > len(data):
        raise ValueError(f'{path}: not a RomFS index')
    files = {}
    for i in range(count):
        offset, size, name, length = struct.unpack_from('<QQII', data, 24 + i*24)
        files[data[strings+name:strings+name+length-1].decode()] = (offset, size)
    return files


def sha256_range(handle, offset, size):
    digest = hashlib.sha256()
    handle.seek(offset)
    while size:
        block = handle.read(min(CHUNK, size))
        if not block:
            raise ValueError('RomFS image ends early')
        digest.update(block)
        size -= len(block)
    return digest.hexdigest()


TRPFD = 'arc/data.trpfd'
GENERATED = 'arc/.data.trpfd-generated'


def fnv1a64(text):
    value = 0xcbf29ce484222645
    for byte in text.encode():
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return value


def trpfd_hashes(data):
    """(offset, count) of the file-hash list: field 0 of the index's root table, a sorted vector of uint64."""
    root = struct.unpack_from('<I', data, 0)[0]
    vtable = root - struct.unpack_from('<i', data, root)[0]
    field = struct.unpack_from('<H', data, vtable + 4)[0]
    vector = root + field + struct.unpack_from('<I', data, root + field)[0]
    return vector + 4, struct.unpack_from('<I', data, vector)[0]


def unpack_trinity(mod, romfs, index):
    """Write <mod>/arc/data.trpfd with the mod's packed files taken out of the hash list; returns their paths."""
    if (mod/TRPFD).is_file() and not (mod/GENERATED).is_file():
        return []
    offset, size = index[TRPFD]
    with (romfs/'romfs.bin').open('rb') as image:
        image.seek(offset)
        data = bytearray(image.read(size))
    start, count = trpfd_hashes(data)
    hashes = list(struct.unpack_from(f'<{count}Q', data, start))
    if any(a >= b for a, b in zip(hashes, hashes[1:])):
        raise ValueError(f'{TRPFD}: the hash list is not sorted')
    position = {h: i for i, h in enumerate(hashes)}
    taken = []
    for file in sorted(p for p in mod.rglob('*') if p.is_file()):
        name = file.relative_to(mod).as_posix()
        i = position.get(fnv1a64(name))
        if name in index or i is None:
            continue
        if i + 1 < count and hashes[i + 1] == hashes[i] + 1:
            raise ValueError(f'{name}: the next hash in {TRPFD} is one higher; this file cannot be taken out')
        struct.pack_into('<Q', data, start + i*8, hashes[i] + 1)
        taken.append(name)
    if taken:
        (mod/'arc').mkdir(exist_ok=True)
        (mod/TRPFD).write_bytes(data)
        (mod/GENERATED).write_text('made by scripts/make_mod_manifest.py from the original index\n')
    elif (mod/GENERATED).is_file():
        (mod/TRPFD).unlink(missing_ok=True)
        (mod/GENERATED).unlink()
    return taken


def manifest(mod, romfs):
    """The manifest lines for every file in the mod folder; files equal to their original are left out.
    Returns (lines, same, added, misspelt): misspelt pairs a mod path with the RomFS path that differs only in case."""
    index = read_index(romfs/'runtime-index.bin')
    for name in unpack_trinity(mod, romfs, index):
        print(f'read loose instead of from arc/data.trpfs: {name}')
    folded = {name.lower(): name for name in index}
    lines, same, added, misspelt = [], [], [], []
    with (romfs/'romfs.bin').open('rb') as image:
        for file in sorted(p for p in mod.rglob('*') if p.is_file()):
            name = file.relative_to(mod).as_posix()
            if name == 'manifest.tsv' or any(part.startswith('.') for part in file.relative_to(mod).parts):
                continue
            if name not in index:
                if name.lower() in folded:
                    misspelt.append((name, folded[name.lower()]))
                    continue
                with file.open('rb') as handle:
                    lines.append(f'{name}\t{file.stat().st_size}\t{sha256_range(handle, 0, file.stat().st_size)}\t-')
                added.append(name)
                continue
            offset, size = index[name]
            with file.open('rb') as handle:
                replacement = sha256_range(handle, 0, file.stat().st_size)
            original = sha256_range(image, offset, size)
            if replacement == original:
                same.append(name)
                continue
            lines.append(f'{name}\t{file.stat().st_size}\t{replacement}\t{original}')
    return lines, same, added, misspelt


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder, files under their RomFS paths')
    parser.add_argument('--romfs', type=Path, required=True,
                        help='a folder holding romfs.bin and runtime-index.bin: <work>/extracted/<version>/romfs')
    args = parser.parse_args()
    romfs = args.romfs
    for need in ('romfs.bin', 'runtime-index.bin'):
        if not (romfs/need).is_file():
            parser.error(f'{romfs/need} not found: run make prepare for that version first')
    if not args.mod.is_dir():
        parser.error(f'{args.mod} is not a folder')
    lines, same, added, misspelt = manifest(args.mod, romfs)
    for name in same:
        print(f'left out (same as the original): {name}')
    if misspelt:
        for name, real in misspelt:
            print(f'not a file of this RomFS: {name} (it has {real}; paths are case-sensitive)', file=sys.stderr)
        sys.exit('nothing written: rename the files above')
    if added:
        print(f'{len(added)} files the RomFS lacks are added, for example {added[0]}')
    if not lines:
        sys.exit('nothing written: the folder replaces or adds no file')
    (args.mod/'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256 (generated by scripts/make_mod_manifest.py)\n'
                                         + '\n'.join(lines) + '\n')
    print(f'{args.mod/"manifest.tsv"}: {len(lines)} files ({len(lines) - len(added)} replaced, {len(added)} added)')


if __name__ == '__main__':
    main()
