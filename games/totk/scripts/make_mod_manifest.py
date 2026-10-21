#!/usr/bin/env python3
"""Write the manifest.tsv of a player's RomFS mod (runtime/romfs.c loads it from TOTK_ROMFS_MODS, which the launcher sets to
~/Library/Application Support/MacNativeGames/TOTK/mods/<version>).

The mod folder holds the replacement files under their RomFS paths (for example Sound/Resource/Stream/Dm_OP_0001.bwav).
Each line of the manifest names a path, the replacement's size and SHA-256 and the original's SHA-256; the game checks
all of them at start, so a mod made for another game version stops the start instead of loading. A file the RomFS lacks
is added (its original's SHA-256 is "-"), unless a RomFS path differs from it only in case: that is a misspelt
replacement (paths are case-sensitive), so nothing is written. The originals come
from the RomFS the app was built with: the app's Contents/Resources/totk/extracted/<version>/romfs (romfs.bin and
runtime-index.bin), or a build work folder's extracted/<version>/romfs.

A mod file the build's own RomFS overlay also replaces (extracted/<version>/romfs-patches, scripts/make_romfs_patches.py)
would stop the start, since two overlays may not replace one file. Translations replace the UI archive
Common.Product.<n>.Nin_NX_NVN.blarc.zs for its textures with words, and the build moves the quick-menu capture keys in
the same archive (issue #73). So the build's edit is applied to the mod's archive with the same checks as
make_romfs_patches.py: the mod must not have changed the members the build patches, and the result must hold exactly
the build's patched members. The merged archive takes the mod file's place (the mod's own file is kept next to it as
<name>.mod-original, and used again on the next run), and its line names the SHA-256 of the build's patched file as the
original: runtime/romfs.c then lets it replace the build's overlay. Any other collision still stops the start.

usage: make_mod_manifest.py <mod folder> (--app <Tears of the Kingdom.app> | --romfs <folder>) [--version v143]
"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

VERSIONS = ('v0', 'v112', 'v121', 'v143')
CHUNK = 8 << 20
KEPT = '.mod-original'   # the mod's own copy of a file merged with the build's patch


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


def manifest(mod, romfs, merged=None):
    """The manifest lines for every file in the mod folder; files equal to their original are left out. `merged` maps
    the paths merged with the build's patch to the SHA-256 of the build's file, which their lines name as the original.
    Returns (lines, same, added, misspelt): misspelt pairs a mod path with the RomFS path that differs only in case."""
    merged = merged or {}
    index = read_index(romfs/'runtime-index.bin')
    folded = {name.lower(): name for name in index}
    lines, same, added, misspelt = [], [], [], []
    with (romfs/'romfs.bin').open('rb') as image:
        for file in sorted(p for p in mod.rglob('*') if p.is_file()):
            name = file.relative_to(mod).as_posix()
            if name == 'manifest.tsv' or name.endswith(KEPT) or any(part.startswith('.') for part in file.relative_to(mod).parts):
                continue
            if name in merged:
                with file.open('rb') as handle:
                    lines.append(f'{name}\t{file.stat().st_size}\t{sha256_range(handle, 0, file.stat().st_size)}\t{merged[name]}')
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


def build_patches(romfs):
    """The build's own RomFS overlay next to this RomFS (extracted/<version>/romfs-patches): its folder and path -> SHA-256."""
    folder = romfs.parent/'romfs-patches'
    patches = {}
    if (folder/'manifest.tsv').is_file():
        for line in (folder/'manifest.tsv').read_text().splitlines():
            if line and not line.startswith('#'):
                path, _, sha, _ = line.split('\t')
                patches[path] = sha
    return folder, patches


def merge_build_patch(mod, name, romfs, index, folder, build_sha):
    """Apply the build's edit (make_romfs_patches.py) to the mod's copy of the UI archive `name` and write the result in its
    place; the mod's own file is kept as <name>.mod-original. False when the mod's file is the original (nothing to merge);
    raises ValueError when the two can't be merged."""
    target, kept = mod/name, mod/(name + KEPT)
    if target.is_file() and not kept.is_file():
        offset, size = index[name]
        with (romfs/'romfs.bin').open('rb') as image, target.open('rb') as handle:
            if sha256_range(handle, 0, target.stat().st_size) == sha256_range(image, offset, size):
                return False   # the original itself: the manifest leaves it out
    try:
        import compression.zstd as zstd   # Python 3.14
    except ImportError:
        raise ValueError(f'{name}: merging it with the build\'s patch needs Python 3.14 (compression.zstd)') from None
    from make_romfs_patches import MEMBERS, common_archive, patch_capture, sarc_members
    if name not in {common_archive(v) for v in VERSIONS}:
        raise ValueError(f'{name}: the build patches this file too, and only the UI archive Common.Product.<n> can be merged; '
                         'leave it out of the mod')
    build_raw = (folder/name).read_bytes()
    if hashlib.sha256(build_raw).hexdigest() != build_sha:
        raise ValueError(f'{folder/name} does not match the build\'s manifest')
    with (romfs/'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]; image.seek(offset); data = image.read(size); assert len(data) == size; return data
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        original_raw = read(name)
    dictionaries = {}
    for lo, hi in sarc_members(pack).values():
        d = zstd.ZstdDict(pack[lo:hi]); dictionaries[d.dict_id] = d
    def unpack(raw):
        info = zstd.get_frame_info(raw)
        if info.dictionary_id and info.dictionary_id not in dictionaries:
            raise ValueError(f'{name}: compressed with a zstd dictionary the game does not have')
        return bytearray(zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id]) if info.dictionary_id else zstd.decompress(raw))
    original, build = unpack(original_raw), unpack(build_raw)
    om, bm = sarc_members(original), sarc_members(build)
    if any(original[slice(*om[k])] != build[slice(*bm[k])] for k in om if k not in MEMBERS) or om.keys() != bm.keys():
        raise ValueError(f'{name}: the build\'s patch changes more than {", ".join(MEMBERS)}; this tool merges only those')
    if target.is_file():   # the mod's own file, unless it is an earlier merge (it holds the build's edit) and the copy is kept
        candidate = unpack(target.read_bytes()); cm = sarc_members(candidate)
        if not kept.is_file() or not all(k in cm and candidate[slice(*cm[k])] == build[slice(*bm[k])] for k in MEMBERS):
            target.replace(kept)
    if not kept.is_file():
        raise ValueError(f'{name}: {kept.name} is missing, so the mod\'s own archive is unknown; copy the mod\'s file in again')
    blarc = unpack(kept.read_bytes()); members = sarc_members(blarc)
    for k in MEMBERS:
        if k not in members or blarc[slice(*members[k])] != original[slice(*om[k])]:
            raise ValueError(f'{name}: the mod changes {k}, which the build patches too, so the two can\'t be merged')
    patch_capture(blarc, members, 0.0)
    for k in MEMBERS:
        if blarc[slice(*members[k])] != build[slice(*bm[k])]:
            raise ValueError(f'{name}: after the edit, {k} differs from the build\'s (another capture frame?); not merged')
    dictionary = zstd.get_frame_info(build_raw).dictionary_id
    data = zstd.compress(bytes(blarc), level=19, zstd_dict=dictionaries[dictionary]) if dictionary else zstd.compress(bytes(blarc), level=19)
    if unpack(data) != blarc:
        raise ValueError(f'{name}: the merged archive does not decompress to itself')
    target.write_bytes(data)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('mod', type=Path, help='the mod folder, files under their RomFS paths')
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
    folder, patches = build_patches(romfs)
    merged = {}
    for name, build_sha in patches.items():
        if (args.mod/name).is_file() or (args.mod/(name + KEPT)).is_file():
            try:
                if not merge_build_patch(args.mod, name, romfs, read_index(romfs/'runtime-index.bin'), folder, build_sha):
                    continue
            except ValueError as error:
                sys.exit(f'nothing written: {error}')
            merged[name] = build_sha
            print(f'merged with the build\'s patch: {name} (the mod\'s own file: {name}{KEPT})')
    lines, same, added, misspelt = manifest(args.mod, romfs, merged)
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
