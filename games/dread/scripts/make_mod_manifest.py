#!/usr/bin/env python3
"""Write the manifest.tsv of a player's RomFS mod for Metroid Dread (runtime/romfs.c loads it from TOTK_ROMFS_MODS, which
the launcher sets to each chosen mods/<name>/romfs next to the work folder; `make mods` runs this for every one).

The mod folder holds the replacement files under their RomFS paths. Each line of the manifest names a path, the
replacement's size and SHA-256 and the original's SHA-256; the game checks all of them at start, so a mod made for
another game version stops the start instead of loading. A file the RomFS lacks is added (its original's SHA-256 is
"-"), unless a RomFS path differs from it only in case: that is a misspelt replacement (paths are case-sensitive), so
nothing is written. The originals come from the RomFS the game was prepared with, which stays in the work folder (the app
holds only the launcher): <work>/extracted/v100/romfs (romfs.bin and runtime-index.bin), ../Dread_build by default.

usage: make_mod_manifest.py <mod folder> [--romfs <folder>]
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

DREAD = Path(__file__).resolve().parents[1]
DEFAULT_ROMFS = DREAD.parent/'Dread_build/extracted/v100/romfs'   # the Makefile's WORK ?= ../Dread_build, version 1.0.0
CHUNK = 8 << 20


def read_index(path):
    """RomFS path -> (offset, size) from runtime-index.bin (scripts/prepare_romfs_backend.py writes it)."""
    data = path.read_bytes()
    magic, count, strings = struct.unpack_from('<8sQQ', data, 0)
    if magic != b'TOTKRFS1' or strings != 24 + count*24 or strings > len(data):
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


def manifest(mod, romfs):
    """The manifest lines for every file in the mod folder; files equal to their original are left out.
    Returns (lines, same, added, misspelt): misspelt pairs a mod path with the RomFS path that differs only in case."""
    index = read_index(romfs/'runtime-index.bin')
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
    parser.add_argument('--romfs', type=Path, default=DEFAULT_ROMFS,
                        help=f'a folder holding romfs.bin and runtime-index.bin (default: {DEFAULT_ROMFS.relative_to(DREAD.parent)})')
    args = parser.parse_args()
    romfs = args.romfs
    for need in ('romfs.bin', 'runtime-index.bin'):
        if not (romfs/need).is_file():
            parser.error(f'{romfs/need} not found: run make prepare first, or name the folder with --romfs')
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
