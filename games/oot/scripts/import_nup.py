#!/usr/bin/env python3
"""Import a verified original NUP into this port's native ExeFS/RomFS layout. Game bytes stay in WORK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'third_party/nup'))
from nup.read import NupReader
from nup.access import iter_children


def entries(reader, directory, directories=None):
    stack = [(directory, '')]
    seen = set()
    while stack:
        index, prefix = stack.pop()
        if index in seen:
            raise ValueError('directory graph is not a tree')
        seen.add(index)
        names = set()
        for node in iter_children(reader, index):
            name = node.name.decode('utf-8')
            if not name or name in ('.', '..') or any(c in name for c in '/\\\0') or name in names:
                raise ValueError(f'unsafe or duplicate node name: {name!r}')
            names.add(name)
            path = prefix + name
            if node.is_dir:
                if directories is not None: directories.append(path)
                stack.append((node.index, path + '/'))
            else:
                yield path, node.index


def package_files(reader, version):
    packages = [p for p in reader.packages if p.version == version and p.kind in (0, 1)]
    if len(packages) != 1:
        raise ValueError(f'expected one application package at version {version}, found {len(packages)}')
    package = packages[0]
    if package.title_id & ~0xfff != 0x0400486024dec000:
        raise ValueError('NUP package is not Ocarina of Time')
    directories = []
    all_files = sorted(entries(reader, package.root_dir, directories))
    directories = sorted(p.removeprefix("romfs/") for p in directories if p.startswith("romfs/"))
    code = [(p, i) for p, i in all_files if p.startswith('exefs/')]
    data = [(p.removeprefix('romfs/'), i) for p, i in all_files if p.startswith('romfs/')]
    if not code or not data:
        raise ValueError('package has no complete ExeFS/RomFS')
    return code, data, directories


def extraction_output(output):
    output = Path(output).resolve()
    for source_root in (ROOT, *ROOT.parents):
        if (source_root / '.git').exists() and (output == source_root or source_root in output.parents):
            raise ValueError('game data must stay outside every source checkout')
    return output


def import_package(source, output, version=131072):
    output = extraction_output(output)
    with NupReader(str(source)) as reader:
        code, data, directories = package_files(reader, version)
        output.mkdir(parents=True, exist_ok=True)
        manifest = []
        exefs = output / 'exefs'; exefs.mkdir(exist_ok=True)
        for path, index in code:
            payload = reader.read_file(index)
            sha = hashlib.sha256(payload).digest()
            if sha != reader.files[index][2]:
                raise ValueError(f'corrupt ExeFS: {path}')
            target = output / path; target.parent.mkdir(parents=True, exist_ok=True)
            if target.exists() and target.read_bytes() != payload:
                raise ValueError(f'output already contains different code: {target}')
            if not target.exists():
                target.write_bytes(payload)
            manifest.append(dict(name=path.removeprefix('exefs/'), size=len(payload), sha256=sha.hex()))
        (output / 'extraction-manifest.json').write_text(json.dumps(dict(source=str(source),container='NUP',
            toc_sha256=reader.super['toc_sha256'].hex(),files=manifest),indent=2)+'\n')
        romfs = output / 'romfs'; romfs.mkdir(exist_ok=True)
        source_identity = dict(toc_sha256=reader.super['toc_sha256'].hex(),version=version)
        previous = romfs / 'index.json'
        if previous.exists():
            old = json.loads(previous.read_text())
            image = romfs / 'romfs.bin'
            if old.get('source_identity') == source_identity and image.is_file() and image.stat().st_size == old['romfs_size']:
                old['directories'] = directories
                previous.write_text(json.dumps(old)+'\n')
                print('reusing the verified NUP RomFS',flush=True)
                return old
            raise ValueError('existing RomFS has different provenance; use another work directory')
        needed = sum((reader.files[i][1]+15)//16*16 for _, i in data)
        if shutil.disk_usage(romfs).free < needed + (3 << 30):
            raise ValueError(f'not enough disk space: need {needed/2**30:.1f} GiB plus reserve')
        image = romfs / 'romfs.bin'; partial = romfs / 'romfs.bin.partial'
        if image.exists() or partial.exists():
            raise ValueError('existing image/partial is left untouched; use another work directory')
        digest = hashlib.sha256(); offset = 0; rows = []; start = last = time.monotonic()
        with partial.open('xb') as f:
            for count, (path, index) in enumerate(data,1):
                payload = reader.read_file(index)
                if hashlib.sha256(payload).digest() != reader.files[index][2]:
                    raise ValueError(f'corrupt RomFS file: {path}')
                padding = (-offset) % 16
                if padding:
                    f.write(bytes(padding)); digest.update(bytes(padding)); offset += padding
                rows.append(dict(path=path,offset=offset,size=len(payload)))
                f.write(payload); digest.update(payload); offset += len(payload)
                if time.monotonic()-last > 15:
                    print(f'RomFS: {count}/{len(data)} files, {offset/2**30:.2f} GiB, {time.monotonic()-start:.0f}s',flush=True)
                    last = time.monotonic()
        partial.replace(image)
        result = dict(status='pass',source=str(source),source_identity=source_identity,integrity='NUP TOC and every uncompressed file SHA256 verified',
                      romfs_size=offset,romfs_sha256=digest.hexdigest(),files=rows,directories=directories)
        previous.write_text(json.dumps(result)+'\n')
        print(f'Verified RomFS: {len(rows)} files, {offset/2**30:.2f} GiB, SHA256 {digest.hexdigest()}',flush=True)
        return result


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--content-version',type=int,default=131072)
    a=p.parse_args();import_package(a.source,a.output,a.content_version)


if __name__=='__main__':
    main()
