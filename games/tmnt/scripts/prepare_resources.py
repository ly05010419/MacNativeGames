#!/usr/bin/env python3
"""Create private host resource tables from a verified, user-owned RomFS.

The output directory must be outside the source checkout. The image is read
only and is not copied; the generated index contains paths and offsets.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from extract_exefs import validate_private_output
from rewrite_modules import REPO, sha256


def prepare(romfs: Path, output: Path, fonts: Path | None = None):
    validate_private_output(output)
    if output == REPO or output.is_relative_to(REPO):
        raise ValueError('generated resources must stay outside the repository')
    manifest = json.loads((romfs / 'index.json').read_text())
    image = romfs / 'romfs.bin'
    size = image.stat().st_size
    if (manifest.get('status') != 'pass' or size != manifest.get('romfs_size')
            or sha256(image) != manifest.get('romfs_sha256')):
        raise ValueError('RomFS differs from its verified extraction index')
    entries = manifest['files']
    strings, rows = bytearray(), bytearray()
    previous = None
    for item in entries:
        name = item['path']
        offset, length = item['offset'], item['size']
        if (not isinstance(name, str) or not name or name.startswith('/')
                or '\\' in name or any(part in ('', '.', '..') for part in name.split('/'))
                or (previous is not None and previous >= name)
                or not isinstance(offset, int) or not isinstance(length, int)
                or offset < 0 or length < 0 or offset > size or length > size - offset):
            raise ValueError('invalid or unsorted RomFS index entry')
        encoded = name.encode('utf-8') + b'\0'
        rows.extend(struct.pack('<QQII', offset, length, len(strings), len(encoded)))
        strings.extend(encoded)
        previous = name
    index = struct.pack('<8sQQ', b'TOTKRFS1', len(entries), 24 + len(rows)) + rows + strings
    if sha256(image) != manifest['romfs_sha256']:
        raise ValueError('RomFS changed during index generation')
    font_rows = ['{"",0ULL,0ULL,""}'] * 6
    if fonts is not None:
        source = json.loads((fonts / 'manifest.json').read_text())
        if source.get('status') != 'pass' or [f['type'] for f in source['fonts']] != list(range(6)):
            raise ValueError('shared font manifest is not verified')
        font_rows = []
        for f in source['fonts']:
            if (not isinstance(f['name'], str) or not f['name']
                    or any(c in f['name'] for c in '/\\"\n\r')):
                raise ValueError('unsafe shared font name')
            name = f"{f['type']}-{f['name']}.bfttf"
            path = fonts / name
            if (path.stat().st_size != f['bfttf_bytes']
                    or f['bfttf_bytes'] != f['font_bytes'] + 8
                    or sha256(path) != f['sha256']):
                raise ValueError('shared font differs from manifest')
            font_rows.append(f'{{"shared-fonts/{name}",{f["bfttf_bytes"]}ULL,{f["font_bytes"]}ULL,"{f["sha256"]}"}}')
    output.mkdir(parents=True, exist_ok=True)
    (output / 'runtime-index.bin').write_bytes(index)
    (output / 'romfs_config.h').write_text(
        f'#define ROMFS_IMAGE_SIZE {size}ULL\n'
        f'#define ROMFS_IMAGE_SHA256 "{manifest["romfs_sha256"]}"\n'
        f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n')
    (output / 'shared_fonts_config.h').write_text(
        '#define SHARED_FONT_COUNT 6\n#define SHARED_FONT_ROWS ' + ','.join(font_rows) + '\n')
    return {'files': len(entries), 'index_sha256': hashlib.sha256(index).hexdigest(),
            'fonts': 0 if fonts is None else 6}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--fonts', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.romfs.resolve(), args.out.resolve(),
                             args.fonts.resolve() if args.fonts else None)))


if __name__ == '__main__':
    main()
