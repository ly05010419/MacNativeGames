#!/usr/bin/env python3
"""Index an already extracted Mario Kart 8 Deluxe RomFS image for the runtime (runtime/romfs.c).

    python3 scripts/index_romfs_image.py <work>/extracted/local/romfs [--sha256 HEX]

The folder holds `romfs.bin`. Two layouts are read:
 - a Nintendo RomFS (header size 0x50 at +0: the directory and file tables of the base game merged with its update), as
   the Zelda-line extraction scripts write it;
 - a flat image with the DMROMFS1 file table (a 32-byte header: magic, table offset, table size, entry count; then
   24-byte entries in breadth-first order, directories `a` = first child, `b` = children, files `a` = absolute offset,
   `b` = size, and a name pool), the shape another build of this game on this Mac left its merged data in. Reading it
   saves extracting the 12 GB again; it is game data, not code.

Writes `index.json` (status, romfs_size, romfs_sha256, files [{path, offset, size}] sorted by path, offsets absolute in
romfs.bin), the same manifest games/odyssey's merge writes, which scripts/prepare_runtime.py turns into the runtime
index. The SHA-256 of 12 GB takes a while: pass it with --sha256 when it is already known.

--extract DIR writes every file out under DIR instead (the add-on content's two files, which the runtime mounts as a
folder: runtime/fs_host.c MountAddOnContent).
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

ENTRY = struct.Struct('<QQIHBB')   # a, b, name offset, name length, type (0 directory, 1 file), pad


def dmromfs_files(f, size):
    head = f.read(32)
    magic, table, table_size, count = struct.unpack('<8sQQQ', head)
    if magic != b'DMROMFS1':
        raise ValueError('not a DMROMFS1 image')
    if table < 32 or table_size != size - table or not 0 < count <= table_size // ENTRY.size:
        raise ValueError('DMROMFS1 table out of bounds')
    f.seek(table)
    raw = f.read(table_size)
    names = raw[count * ENTRY.size:]
    entries = [ENTRY.unpack_from(raw, i * ENTRY.size) for i in range(count)]
    if entries[0][4] != 0 or entries[0][3] != 0:
        raise ValueError('DMROMFS1 root is not an unnamed directory')
    paths = {0: ''}
    files = []
    for k, (a, b, name, nlen, kind, pad) in enumerate(entries):
        if pad or kind > 1 or name + nlen > len(names):
            raise ValueError(f'DMROMFS1 entry {k} malformed')
        if kind == 0:
            if not (k < a <= count and b <= count - a) and b:
                raise ValueError(f'DMROMFS1 directory {k} children out of range')
            for child in range(a, a + b):
                cname = names[entries[child][2]:entries[child][2] + entries[child][3]].decode('utf-8')
                if not cname or '/' in cname or cname in ('.', '..'):
                    raise ValueError(f'DMROMFS1 entry {child} has a bad name')
                paths[child] = paths[k] + '/' + cname
        else:
            if k not in paths:
                raise ValueError(f'DMROMFS1 file {k} unreachable')
            if a < 32 or a > table or b > table - a:
                raise ValueError(f'DMROMFS1 file {paths[k]} outside the data')
            files.append(dict(path=paths[k].lstrip('/'), offset=a, size=b))
    return files


def nintendo_files(f, size):
    """A Nintendo RomFS: header (0x50), directory and file tables; file data at data_off + offset."""
    head = f.read(0x50)
    header_size, = struct.unpack_from('<Q', head, 0)
    if header_size != 0x50:
        raise ValueError('not a Nintendo RomFS')
    (_, _dh_off, _dh_size, dir_off, dir_size, _fh_off, _fh_size, file_off, file_size,
     data_off) = struct.unpack_from('<10Q', head, 0)
    f.seek(dir_off)
    dirs = f.read(dir_size)
    f.seek(file_off)
    ftab = f.read(file_size)

    def dname(o):
        nlen, = struct.unpack_from('<I', dirs, o + 0x14)
        return dirs[o + 0x18:o + 0x18 + nlen].decode('utf-8')

    out = []

    def walk(o, prefix):
        _parent, _sibling, child, first_file, _hash, _nlen = struct.unpack_from('<6I', dirs, o)
        fo = first_file
        while fo != 0xFFFFFFFF:
            _p, sibling, off, fsize, _h, nlen = struct.unpack_from('<IIQQII', ftab, fo)
            name = ftab[fo + 0x20:fo + 0x20 + nlen].decode('utf-8')
            if data_off + off + fsize > size:
                raise ValueError(f'RomFS file {prefix}{name} outside the image')
            out.append(dict(path=f'{prefix}{name}', offset=data_off + off, size=fsize))
            fo = sibling
        co = child
        while co != 0xFFFFFFFF:
            walk(co, f'{prefix}{dname(co)}/')
            co, = struct.unpack_from('<I', dirs, co + 4)

    walk(0, '')
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while chunk := f.read(16 << 20):
            h.update(chunk)
    return h.hexdigest()


def index(folder, sha=None):
    image = folder / 'romfs.bin'
    size = image.stat().st_size
    with image.open('rb') as f:
        magic = f.read(8)
        f.seek(0)
        files = dmromfs_files(f, size) if magic == b'DMROMFS1' else nintendo_files(f, size)
    files.sort(key=lambda x: x['path'].encode())
    for a, b in zip(files, files[1:]):
        if a['path'] == b['path']:
            raise ValueError(f'duplicate path {a["path"]}')
    return dict(status='pass', layout='DMROMFS1' if magic == b'DMROMFS1' else 'romfs', romfs_size=size,
                romfs_sha256=sha or sha256_file(image), files=files)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('folder', type=Path)
    ap.add_argument('--sha256', help='the image SHA-256 when already known (skips hashing 12 GB)')
    ap.add_argument('--extract', type=Path, help='write the files out under this folder (small images only)')
    args = ap.parse_args()
    if args.extract:
        manifest = index(args.folder.resolve(), args.sha256)
        if sum(f['size'] for f in manifest['files']) > 256 << 20:
            raise SystemExit('--extract is for small images (add-on content), not the game data')
        with (args.folder / 'romfs.bin').open('rb') as src:
            for f in manifest['files']:
                out = args.extract / f['path']
                if out.resolve().parent != (args.extract.resolve() / f['path']).parent:
                    raise SystemExit(f'bad path {f["path"]}')
                out.parent.mkdir(parents=True, exist_ok=True)
                src.seek(f['offset'])
                out.write_bytes(src.read(f['size']))
        print(json.dumps(dict(status='pass', extracted=len(manifest['files']), to=str(args.extract))))
        return
    manifest = index(args.folder.resolve(), args.sha256)
    (args.folder / 'index.json').write_text(json.dumps(manifest, indent=1) + '\n')
    print(json.dumps(dict(status='pass', layout=manifest['layout'], files=len(manifest['files']),
                          size=manifest['romfs_size'])))


if __name__ == '__main__':
    main()
