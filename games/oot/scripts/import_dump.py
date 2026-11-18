#!/usr/bin/env python3
"""Import a dumped game folder: its ExeFS into extracted/<version>/exefs with extraction-manifest.json (the files
extract_exefs.py writes from an NSP), and its RomFS into extracted/<version>/romfs/romfs.bin and index.json (the files
extract_romfs.py writes from an NCA; prepare_romfs_backend.py then builds the runtime index from them).

A dump folder (base/, updates/<version>/, dlc/, as Switch 2 dumpers write it: Donkey Kong Bananza) has no NCA, so there is
no IVFC tree to verify: the image is the folder's files one after another (each 16-byte aligned), sorted by path as
extract_romfs.py sorts them, and the runtime reads them by the index's offsets. An update's RomFS in such a dump is the
patched tree (the base game's files with the update's applied), so it is packed alone, as nxnative's pack_dump.py does
(seedds/nxnative 8dd0c98a, MIT). The image's SHA-256 is computed while it is written; the runtime checks the image
against it (romfs-config.txt).

A dump's ExeFS holds the decrypted NSO files; their segment hashes are checked by prepare_modules.py, as for an NSP.
The newest update's ExeFS and RomFS are taken (updates/<highest version>/), else the base game's.

usage: import_dump.py <dump folder> --output extracted/<version> [--update <version>]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

ALIGN = 16
CHUNK = 8 * 1024 * 1024


def not_game_file(name):
    """Files a dump folder may hold that are no game file: the dumper's own listing of each folder (.listing) and what
    macOS writes into a folder Finder showed (.DS_Store, and ._<name> AppleDouble files on volumes without extended
    attributes). 2026-11-12: Bananza's dump, opened in Finder after it was dumped, held 14 .DS_Store files, and the
    packed RomFS no longer matched the verified one; no file of the game differed (games/totk/scripts/import_dump.py)."""
    return name in ('.listing', '.DS_Store') or name.startswith('._')


def walk(folder):
    """Every regular file under folder as (RomFS path, size), sorted as extract_romfs.py sorts them. Symbolic links,
    names with a backslash or a NUL, and anything that is not a regular file stop the pack."""
    files = []
    for top, dirs, names in os.walk(folder, followlinks=False):
        dirs.sort()
        for name in names:
            if not_game_file(name):
                continue
            full = Path(top) / name
            rel = full.relative_to(folder).as_posix()
            if full.is_symlink() or not full.is_file():
                raise SystemExit(f'not a regular file: {rel}')
            if '\\' in rel or '\0' in rel or name in ('.', '..'):
                raise SystemExit(f'unsafe RomFS path: {rel!r}')
            files.append((rel, full.stat().st_size))
    for top, dirs, _ in os.walk(folder, followlinks=False):
        for d in dirs:
            if (Path(top) / d).is_symlink():
                raise SystemExit(f'symbolic link folder: {(Path(top) / d).relative_to(folder)}')
    return sorted(files)


def pack(folder, output):
    files = walk(folder)
    if not files:
        raise SystemExit(f'{folder}: no files')
    output.mkdir(parents=True, exist_ok=True)
    image = output / 'romfs.bin'
    partial = image.with_suffix('.partial')
    digest = hashlib.sha256()
    rows = []
    offset = 0
    with partial.open('wb') as out:
        for rel, size in files:
            pad = -offset % ALIGN
            if pad:
                out.write(bytes(pad))
                digest.update(bytes(pad))
                offset += pad
            done, own = 0, hashlib.sha256()
            with (folder / rel).open('rb') as f:
                while chunk := f.read(CHUNK):
                    out.write(chunk)
                    digest.update(chunk)
                    own.update(chunk)
                    done += len(chunk)
            if done != size:
                raise SystemExit(f'{rel}: changed while packing ({size} -> {done} bytes)')
            rows.append(dict(path=rel, offset=offset, size=size, sha256=own.hexdigest()))   # romfs_pin.py checks it
            offset += size
    partial.replace(image)
    directories=sorted(str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_dir())
    report = dict(status='pass', directories=directories, source=str(folder), romfs_size=offset, romfs_sha256=digest.hexdigest(), files=rows,
                  integrity='dumped RomFS folder packed by pack_romfs_folder.py; no NCA, so no IVFC levels to verify')
    (output / 'index.json').write_text(json.dumps(report) + '\n')
    return dict(status='pass', file_count=len(rows), size=offset, sha256=report['romfs_sha256'])


def adopt_image(folder, output, source):
    """An image of this folder packed by an earlier build and kept outside the work folder (the game library,
    build.py adoptable_image), cloned into output instead of packing the folder's 26 GB again (2026-11-12). Its rows are
    the ones pack() writes (the folder's files sorted by path, 16-byte aligned), so its size must be what the folder
    packs to; each file's SHA-256 is read from the image itself, in one pass, and romfs_pin.py then checks every file
    as it does after a pack, so an image that is not this folder's fails there."""
    files = walk(folder)
    rows, offset = [], 0
    for rel, size in files:
        offset += -offset % ALIGN
        rows.append(dict(path=rel, offset=offset, size=size))
        offset += size
    if Path(source).stat().st_size != offset:
        raise SystemExit(f'{source}: {Path(source).stat().st_size} bytes, but {folder} packs to {offset}')
    output.mkdir(parents=True, exist_ok=True)
    image = output / 'romfs.bin'
    partial = image.with_suffix('.partial')
    if subprocess.run(['cp', '-c', str(source), str(partial)]).returncode:   # an APFS clone: no copy on the same volume
        shutil.copyfile(source, partial)
    digest = hashlib.sha256()
    with partial.open('rb') as f:
        position = 0
        for row in rows:
            gap = f.read(row['offset'] - position)
            digest.update(gap)
            own, left = hashlib.sha256(), row['size']
            while left:
                chunk = f.read(min(CHUNK, left))
                if not chunk:
                    raise SystemExit(f'{source}: ends inside {row["path"]}')
                own.update(chunk)
                digest.update(chunk)
                left -= len(chunk)
            row['sha256'] = own.hexdigest()   # romfs_pin.py checks it
            position = row['offset'] + row['size']
    partial.replace(image)
    st = Path(source).stat()
    directories = sorted(str(p.relative_to(folder)) for p in folder.rglob('*') if p.is_dir())
    report = dict(status='pass', directories=directories, source=str(folder), romfs_size=offset, romfs_sha256=digest.hexdigest(),
                  files=rows, adopted_from=str(source), adopted_size=st.st_size, adopted_mtime_ns=st.st_mtime_ns,
                  integrity='an earlier pack of this dump folder, taken from the game library; every file hashed from it '
                            '(romfs_pin.py checks them)')
    (output / 'index.json').write_text(json.dumps(report) + '\n')
    return dict(status='pass', file_count=len(rows), size=offset, sha256=report['romfs_sha256'])


def content_folder(dump, update):
    """updates/<update>/ (the highest version when update is None and there are updates), else base/."""
    updates = dump / 'updates'
    versions = sorted((int(p.name) for p in updates.iterdir() if p.is_dir() and p.name.isdigit()), reverse=True) if updates.is_dir() else []
    if update is not None:
        if update not in versions:
            raise SystemExit(f'{dump}: no updates/{update}')
        return updates / str(update), update
    return (updates / str(versions[0]), versions[0]) if versions else (dump / 'base', 0)


def import_exefs(source, output):
    """Copy the ExeFS files and write extraction-manifest.json as extract_exefs.py does (its 'files' list)."""
    if not (source / 'main.npdm').is_file() or not (source / 'main').is_file():
        raise SystemExit(f'{source}: not an ExeFS folder (no main / main.npdm)')
    target = output / 'exefs'
    if target.exists():
        raise SystemExit(f'{target} exists: remove it to import again')
    target.mkdir(parents=True)
    files = []
    for path in sorted(p for p in source.iterdir() if p.is_file() and not p.name.startswith('.')):   # not a dumper's .listing
        shutil.copyfile(path, target / path.name)
        data = (target / path.name).read_bytes()
        files.append({'name': path.name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    manifest = {'source': str(source), 'container': 'dump folder',
                'integrity': 'dump folder: no NCA; NSO segment hashes are checked by prepare_modules.py', 'files': files}
    (output / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return [f['name'] for f in files]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump', type=Path, help='the dump folder (base/, updates/<version>/, dlc/)')
    parser.add_argument('--output', type=Path, required=True, help='extracted/<version>')
    parser.add_argument('--update', type=int, help='the update version to take (default: the highest one)')
    parser.add_argument('--adopted-image', type=Path, help='an earlier pack of this folder (the game library\'s romfs.bin): '
                        'cloned and hashed instead of packing again (adopt_image)')
    args = parser.parse_args()
    if not args.dump.is_dir():
        parser.error(f'{args.dump}: not a folder')
    folder, version = content_folder(args.dump.resolve(), args.update)
    exefs = (folder / 'exefs') if (folder / 'exefs').is_dir() else (args.dump.resolve() / 'base/exefs')
    modules = import_exefs(exefs, args.output)
    romfs = (adopt_image(folder / 'romfs', args.output / 'romfs', args.adopted_image) if args.adopted_image else
             pack(folder / 'romfs', args.output / 'romfs'))
    print(json.dumps(dict(update=version, exefs=modules, romfs=romfs)))


if __name__ == '__main__':
    main()
