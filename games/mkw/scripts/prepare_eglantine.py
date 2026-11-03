#!/usr/bin/env python3
"""Prepare an extracted MKW ExeFS and indexed NSP RomFS for Eglantine.

The output contains game data and needs roughly 23 GB. It must be a new directory.
Run extract_mkw.py first and provide its ExeFS and romfs/index.json paths.
"""
import argparse
import hashlib
import json
import os
import shutil
import tempfile
from pathlib import Path


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        while True:
            block = f.read(8 << 20)
            if not block:
                return h.hexdigest()
            h.update(block)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--exefs', type=Path, required=True)
    p.add_argument('--index', type=Path, required=True, help='extract_mkw.py romfs/index.json')
    p.add_argument('--out', type=Path, required=True, help='new output directory')
    args = p.parse_args()
    expected = json.loads((Path(__file__).resolve().parents[1] / 'assets/expected-v1.0.0.json').read_text())
    index = json.loads(args.index.read_text())
    if index.get('status') != 'pass' or index.get('title_id', '').upper() != expected['title_id']:
        p.error('the index is not a verified MKW extraction')
    if (index.get('romfs_size'), index.get('romfs_sha256')) != (expected['romfs']['size'], expected['romfs']['sha256']):
        p.error('the RomFS does not match the recorded MKW 1.0.0 image')
    for name, want in expected['exefs_sha256'].items():
        if digest(args.exefs / name) != want:
            p.error(f'ExeFS hash mismatch: {name}')
    out = args.out.absolute()
    if out.exists():
        p.error('output already exists')
    out.parent.mkdir(parents=True, exist_ok=True)
    temp = Path(tempfile.mkdtemp(prefix='.mkw-eglantine-', dir=out.parent))
    try:
        exefs = temp / 'exefs'
        exefs.mkdir()
        for name in expected['exefs_sha256']:
            shutil.copyfile(args.exefs / name, exefs / name)
        remaining = index['romfs_size']
        h = hashlib.sha256()
        with open(index['source'], 'rb') as source, (exefs / 'game.romfs').open('xb') as target:
            source.seek(index['romfs_offset'])
            while remaining:
                data = source.read(min(remaining, 8 << 20))
                if not data:
                    raise ValueError('short read from NSP')
                target.write(data)
                h.update(data)
                remaining -= len(data)
        if h.hexdigest() != expected['romfs']['sha256']:
            raise ValueError('RomFS hash mismatch')
        if out.exists():
            raise FileExistsError(f'output appeared while copying: {out}')
        os.rename(temp, out)
        print(f'Prepared {out / "exefs/main"}')
    finally:
        if temp.exists():
            shutil.rmtree(temp)


if __name__ == '__main__':
    main()
