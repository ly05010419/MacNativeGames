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
from pathlib import Path

ALIGN = 16
CHUNK = 8 * 1024 * 1024


def walk(folder):
    """Every regular file under folder as (RomFS path, size), sorted as extract_romfs.py sorts them. Symbolic links,
    names with a backslash or a NUL, and anything that is not a regular file stop the pack."""
    files = []
    for top, dirs, names in os.walk(folder, followlinks=False):
        dirs.sort()
        for name in names:
            if name == '.listing':   # the dumper's own listing of the folder, not a game file
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
            done = 0
            with (folder / rel).open('rb') as f:
                while chunk := f.read(CHUNK):
                    out.write(chunk)
                    digest.update(chunk)
                    done += len(chunk)
            if done != size:
                raise SystemExit(f'{rel}: changed while packing ({size} -> {done} bytes)')
            rows.append(dict(path=rel, offset=offset, size=size))
            offset += size
    partial.replace(image)
    report = dict(status='pass', source=str(folder), romfs_size=offset, romfs_sha256=digest.hexdigest(), files=rows,
                  integrity='dumped RomFS folder packed by pack_romfs_folder.py; no NCA, so no IVFC levels to verify')
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
    args = parser.parse_args()
    if not args.dump.is_dir():
        parser.error(f'{args.dump}: not a folder')
    folder, version = content_folder(args.dump.resolve(), args.update)
    exefs = (folder / 'exefs') if (folder / 'exefs').is_dir() else (args.dump.resolve() / 'base/exefs')
    modules = import_exefs(exefs, args.output)
    romfs = pack(folder / 'romfs', args.output / 'romfs')
    print(json.dumps(dict(update=version, exefs=modules, romfs=romfs)))


if __name__ == '__main__':
    main()
