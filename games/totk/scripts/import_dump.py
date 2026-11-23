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

A .nup holds the same trees as packages (base, update, add-on), so it packs to the same image, byte for byte; each of its
files is checked against the SHA-256 in the package's table as it is read. Add-on content (--addons) goes the same way, one
image per pack, as runtime/aoc_romfs.c reads them (index = add-on title ID - application title ID - 0x1000; seedds/nxnative
scripts/pack_dump.py, MIT).

usage: import_dump.py <dump folder or .nup> --output extracted/<version> [--update <version>] [--nup-lib <reader folder>]
       import_dump.py <dump folder or .nup> --addons <folder> --title <application ID> [--only <index>] [--nup-lib <folder>]
       import_dump.py <dump folder or .nup> --list-addons --title <application ID> [--nup-lib <folder>]
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ALIGN = 16
CHUNK = 8 * 1024 * 1024
MAX_ADDONS = 2   # runtime/aoc_romfs.c AOC_MAX


def not_game_file(name):
    """Files a dump folder may hold that are no game file: the dumper's own listing of each folder (.listing) and what
    macOS writes into a folder Finder showed (.DS_Store, and ._<name> AppleDouble files on volumes without extended
    attributes). 2026-11-12: Bananza's dump, opened in Finder after it was dumped, held 14 .DS_Store files, and the
    packed RomFS no longer matched the verified one (2df8c87e... instead of 8982312b...); no file of the game differed."""
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


def layout(files):
    """(rows, image size): where each file starts in the image, 16-byte aligned, in the given order. pack() writes the
    files there and index_adopted() reads an image packed the same way, so the two cannot place a file differently."""
    rows, offset = [], 0
    for rel, size in files:
        offset += -offset % ALIGN
        rows.append(dict(path=rel, offset=offset, size=size))
        offset += size
    return rows, offset


class FolderTree:
    """A RomFS as a folder of a dump."""
    integrity = 'dumped RomFS folder packed by pack_romfs_folder.py; no NCA, so no IVFC levels to verify'

    def __init__(self, root):
        self.root = Path(root)
        self.label = str(root)

    def files(self):
        return walk(self.root)

    def chunks(self, rel):
        with (self.root / rel).open('rb') as f:
            while chunk := f.read(CHUNK):
                yield chunk


class NupTree:
    """A RomFS as a directory of a .nup: each file is checked against the SHA-256 in the package's table when it is read."""
    integrity = 'RomFS of a .nup package; every file checked against the SHA-256 in the table'

    def __init__(self, reader, directory, children, label):
        self.reader, self.label = reader, label
        self.index = dict(tree_entries(reader, directory, children))

    def files(self):
        return sorted((rel, self.reader.file_info(i)[1]) for rel, i in self.index.items())

    def chunks(self, rel):
        data = self.reader.read_file(self.index[rel])
        if hashlib.sha256(data).digest() != self.reader.file_info(self.index[rel])[2]:
            raise SystemExit(f'{rel}: damaged in {self.label} (its SHA-256 is not the one in the package table)')
        yield data


def tree_entries(reader, directory, children):
    """(path, file index) of every file under a directory of a .nup. Its names are not trusted: a path of the package never
    leaves the tree, and a directory reached twice stops the import."""
    stack, seen = [(directory, '')], set()
    while stack:
        current, prefix = stack.pop()
        if current in seen:
            raise SystemExit('the .nup\'s directories are not a tree')
        seen.add(current)
        names = set()
        for node in children(reader, current):
            name = node.name.decode('utf-8')
            if not name or name in ('.', '..') or name in names or any(c in name for c in '/\\\0'):
                raise SystemExit(f'unsafe or duplicate name in the .nup: {name!r}')
            names.add(name)
            if node.is_dir:
                stack.append((node.index, f'{prefix}{name}/'))
            elif not not_game_file(name):
                yield prefix + name, node.index


def pack_tree(tree, output, **record):
    """The image of a RomFS tree and its index.json; record: more fields for the index (an add-on pack's index and title)."""
    files = tree.files()
    if not files:
        raise SystemExit(f'{tree.label}: no files')
    rows, total = layout(files)
    output.mkdir(parents=True, exist_ok=True)
    image = output / 'romfs.bin'
    partial = image.with_suffix('.partial')
    digest = hashlib.sha256()
    offset = 0
    with partial.open('wb') as out:
        for row in rows:
            pad = row['offset'] - offset
            if pad:
                out.write(bytes(pad))
                digest.update(bytes(pad))
                offset += pad
            done = 0
            for chunk in tree.chunks(row['path']):
                out.write(chunk)
                digest.update(chunk)
                done += len(chunk)
            if done != row['size']:
                raise SystemExit(f'{row["path"]}: changed while packing ({row["size"]} -> {done} bytes)')
            offset += done
    partial.replace(image)
    report = dict(status='pass', source=tree.label, romfs_size=total, romfs_sha256=digest.hexdigest(), files=rows,
                  integrity=tree.integrity, **record)
    (output / 'index.json').write_text(json.dumps(report) + '\n')
    return dict(status='pass', file_count=len(rows), size=total, sha256=report['romfs_sha256'])


def pack(folder, output):
    return pack_tree(FolderTree(folder), output)


def index_adopted(folder, output, sha256, source):
    """The index of an image build_app.py took from an installed app (adopt_game_data, which checked that its SHA-256 is
    the verified one): the rows pack() writes, from the listing of the folder (or the tree of a .nup), without reading the
    files again. It also records where the image came from, its size and modification time (build_app.py adoptable_bundle)."""
    tree = folder if hasattr(folder, 'files') else FolderTree(folder)
    folder = tree.label
    rows, total = layout(tree.files())
    image = output / 'romfs.bin'
    if image.stat().st_size != total:
        raise SystemExit(f'{image}: {image.stat().st_size} bytes, but {folder} packs to {total}: not this folder\'s image')
    st = Path(source).stat()
    report = dict(status='pass', source=str(folder), romfs_size=total, romfs_sha256=sha256, files=rows,
                  adopted_from=str(source), adopted_size=st.st_size, adopted_mtime_ns=st.st_mtime_ns,
                  integrity='taken from an installed app; its SHA-256 equals the verified image of this folder packed')
    (output / 'index.json').write_text(json.dumps(report) + '\n')
    return dict(status='pass', file_count=len(rows), size=total, sha256=sha256)


def content_folder(dump, update):
    """updates/<update>/ (the highest version when update is None and there are updates), else base/."""
    updates = dump / 'updates'
    versions = sorted((int(p.name) for p in updates.iterdir() if p.is_dir() and p.name.isdigit()), reverse=True) if updates.is_dir() else []
    if update is not None:
        if update not in versions:
            raise SystemExit(f'{dump}: no updates/{update}')
        return updates / str(update), update
    return (updates / str(versions[0]), versions[0]) if versions else (dump / 'base', 0)


def write_exefs(items, label, container, integrity, output):
    """The ExeFS files (name, bytes) and extraction-manifest.json as extract_exefs.py writes it (its 'files' list)."""
    items = sorted(items)
    if not {'main', 'main.npdm'} <= {name for name, _ in items}:
        raise SystemExit(f'{label}: not an ExeFS (no main / main.npdm)')
    target = output / 'exefs'
    if target.exists():
        raise SystemExit(f'{target} exists: remove it to import again')
    target.mkdir(parents=True)
    files = []
    for name, data in items:
        (target / name).write_bytes(data)
        files.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
    manifest = {'source': label, 'container': container, 'integrity': integrity, 'files': files}
    (output / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return [f['name'] for f in files]


def import_exefs(source, output):
    """Copy the ExeFS files of a dump folder (not a dumper's .listing)."""
    items = [(p.name, p.read_bytes()) for p in source.iterdir() if p.is_file() and not p.name.startswith('.')]
    return write_exefs(items, str(source), 'dump folder', 'dump folder: no NCA; NSO segment hashes are checked by '
                       'prepare_modules.py', output)


def addon_index(title, application):
    """The add-on content index of a title ID (title = the application's + 0x1000 + index, as nn::aoc lists it), or None."""
    index = title - application - 0x1000
    return index if 1 <= index <= MAX_ADDONS else None


class FolderGame:
    """A dump folder: base/, updates/<version>/, dlc/<add-on title ID>/<version>/romfs."""

    def __init__(self, dump, update):
        self.dump = dump.resolve()
        self.folder, self.version = content_folder(self.dump, update)
        self.romfs = FolderTree(self.folder / 'romfs')

    def import_exefs(self, output):
        exefs = self.folder / 'exefs'
        return import_exefs(exefs if exefs.is_dir() else self.dump / 'base/exefs', output)

    def addons(self, application):
        found = {}
        for pack in sorted(p for p in (self.dump / 'dlc').iterdir() if p.is_dir()) if (self.dump / 'dlc').is_dir() else []:
            try:
                index = addon_index(int(pack.name, 16), application)
            except ValueError:
                continue
            versions = sorted((p for p in pack.iterdir() if p.name.isdigit() and (p / 'romfs').is_dir()), key=lambda p: int(p.name))
            if index is not None and versions:
                found[index] = (f'{int(pack.name, 16):016X}', FolderTree(versions[-1] / 'romfs'))
        return found

    def close(self):
        pass


class NupGame:
    """A .nup of one game: its base and update packages and its add-on packages. The reader is the one Ocarina of Time's
    build uses; lib is its folder (the work folder holds a copy of games/totk only)."""

    def __init__(self, path, update, lib):
        if str(lib) not in sys.path:
            sys.path.insert(0, str(lib))
        from nup import format as fmt
        from nup.access import iter_children
        from nup.read import NupReader
        self.fmt, self.children, self.path = fmt, iter_children, path
        self.reader = NupReader(str(path))
        apps = [p for p in self.reader.packages if p.kind in (fmt.KIND_BASE, fmt.KIND_UPDATE)]
        chosen = [p for p in apps if update is None or p.version == update]
        if not chosen:
            raise SystemExit(f'{path}: no package of version {update}')
        self.package = max(chosen, key=lambda p: p.version)
        self.base = next((p for p in apps if p.kind == fmt.KIND_BASE), None)
        self.version = self.package.version
        if self.package.romfs_dir == fmt.NO_DIR:
            raise SystemExit(f'{path}: its version {self.version} package has no RomFS')
        self.romfs = NupTree(self.reader, self.package.romfs_dir, iter_children, str(path))

    def import_exefs(self, output):
        package = self.package if self.package.exefs_dir != self.fmt.NO_DIR else self.base
        if package is None or package.exefs_dir == self.fmt.NO_DIR:
            raise SystemExit(f'{self.path}: no ExeFS')
        tree = NupTree(self.reader, package.exefs_dir, self.children, str(self.path))
        if any('/' in name for name in tree.index):
            raise SystemExit(f'{self.path}: its ExeFS holds a folder')
        items = [(name, b''.join(tree.chunks(name))) for name in tree.index]
        return write_exefs(items, str(self.path), 'nup', 'NUP table SHA-256 of every file; NSO segment hashes are checked '
                           'by prepare_modules.py', output)

    def addons(self, application):
        newest = {}
        for p in self.reader.packages:
            if p.kind in (self.fmt.KIND_ADDON, self.fmt.KIND_ADDON_UPDATE) and p.romfs_dir != self.fmt.NO_DIR:
                index = addon_index(p.title_id, application)
                if index is not None and (index not in newest or p.version > newest[index].version):
                    newest[index] = p
        return {i: (f'{p.title_id:016X}', NupTree(self.reader, p.romfs_dir, self.children, f'{self.path} add-on {i}'))
                for i, p in newest.items()}

    def close(self):
        self.reader.close()


def open_game(path, update, nup_lib):
    if path.is_dir():
        return FolderGame(path, update)
    if not path.is_file():
        raise SystemExit(f'{path}: no such file or folder')
    if nup_lib is None:
        raise SystemExit(f'{path}: a .nup needs --nup-lib')
    return NupGame(path.resolve(), update, nup_lib)


def import_addons(game, application, output, wanted=None):
    """Each add-on pack into <output>/<index>/ (romfs.bin, index.json); prepare_romfs_backend.py --aoc writes its runtime index."""
    made = {}
    for index, (title, tree) in sorted(game.addons(application).items()):
        if wanted is not None and index not in wanted:
            continue
        result = pack_tree(tree, output / str(index), index=index, title_id=title)
        made[str(index)] = dict(title_id=title, size=result['size'], sha256=result['sha256'], files=result['file_count'])
    return made


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dump', type=Path, help='the dump folder (base/, updates/<version>/, dlc/) or a .nup package')
    parser.add_argument('--output', type=Path, help='extracted/<version>')
    parser.add_argument('--update', type=int, help='the update version to take (default: the highest one)')
    parser.add_argument('--adopted-image', type=Path, help='the installed app\'s image already placed in --romfs-output '
                        '(build_app.py adopt_game_data): index it instead of packing the folder again')
    parser.add_argument('--sha256', help='with --adopted-image: the SHA-256 build_app.py checked it has')
    parser.add_argument('--romfs-output', type=Path, help='where the RomFS image and index go (default: <output>/romfs)')
    parser.add_argument('--nup-lib', type=Path, help='the folder of the .nup reader (games/oot/third_party/nup)')
    parser.add_argument('--title', help='with --addons or --list-addons: the application\'s title ID (16 hex digits)')
    parser.add_argument('--addons', type=Path, help='only the add-on packs: each into <this folder>/<index>')
    parser.add_argument('--only', type=int, action='append', help='with --addons: this index only (repeatable)')
    parser.add_argument('--list-addons', action='store_true', help='what add-on packs the source holds; reads no file')
    args = parser.parse_args()
    if (args.addons or args.list_addons) and not args.title:
        parser.error('--addons and --list-addons need --title')
    if not (args.addons or args.list_addons or args.output):
        parser.error('--output is required')
    game = open_game(args.dump, args.update, args.nup_lib)
    try:
        if args.list_addons:
            found = game.addons(int(args.title, 16))
            print(json.dumps(dict(addons={str(i): dict(title_id=title, files=len(tree.files()),
                                                       bytes=sum(size for _, size in tree.files()))
                                          for i, (title, tree) in sorted(found.items())})))
            return
        if args.addons:
            print(json.dumps(dict(addons=import_addons(game, int(args.title, 16), args.addons,
                                                       set(args.only) if args.only else None))))
            return
        modules = game.import_exefs(args.output)
        romfs_output = args.romfs_output or args.output / 'romfs'
        if args.adopted_image:
            if not args.sha256:
                parser.error('--adopted-image needs --sha256')
            romfs = index_adopted(game.romfs, romfs_output, args.sha256, args.adopted_image)
        else:
            romfs = pack_tree(game.romfs, romfs_output)
        print(json.dumps(dict(update=game.version, exefs=modules, romfs=romfs)))
    finally:
        game.close()


if __name__ == '__main__':
    main()
