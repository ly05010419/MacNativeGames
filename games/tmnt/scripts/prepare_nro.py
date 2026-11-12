#!/usr/bin/env python3
"""Prepare a registered NRO for experimental AOT rewriting, outside the checkout."""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from extract_exefs import validate_private_output
from rewrite_modules import sha256


def nso_from_nro(nro):
    """Wrap an NRO's contiguous memory segments as an uncompressed NSO input."""
    if len(nro) < 0x80 or nro[0x10:0x14] != b'NRO0':
        raise ValueError('not an NRO')
    size = struct.unpack_from('<I', nro, 0x18)[0]
    segs = [struct.unpack_from('<II', nro, 0x20 + 8 * i) for i in range(3)]
    if (size != len(nro) or segs[0][0] != 0 or
        segs[1][0] != segs[0][1] or segs[2][0] != sum(segs[1]) or
        sum(segs[2]) != size or any(not length for _, length in segs)):
        raise ValueError('invalid NRO segment layout')
    header = bytearray(0x100)
    header[:4] = b'NSO0'
    for i, (offset, length) in enumerate(segs):
        struct.pack_into('<III', header, 0x10 + 16 * i, 0x100 + offset, offset, length)
        struct.pack_into('<I', header, 0x60 + 4 * i, length)
    struct.pack_into('<I', header, 0x3c, struct.unpack_from('<I', nro, 0x38)[0])
    header[0x40:0x60] = nro[0x40:0x60]
    header[0x88:0xa0] = nro[0x68:0x80]
    return bytes(header) + nro


def nrr_hashes(nrr):
    if len(nrr) < 0x348 or nrr[:4] != b'NRR0':
        raise ValueError('invalid NRR')
    offset, count = struct.unpack_from('<II', nrr, 0x340)
    if not 0 < count <= 4096 or offset > len(nrr) or 32 * count > len(nrr) - offset:
        raise ValueError('invalid NRR hash table')
    return {nrr[offset + 32 * i:offset + 32 * (i + 1)] for i in range(count)}


def prepare(romfs, out):
    validate_private_output(out)
    index = json.loads((romfs / 'index.json').read_text())
    image = romfs / 'romfs.bin'
    if index.get('status') != 'pass' or sha256(image) != index.get('romfs_sha256'):
        raise ValueError('RomFS image differs from verified extraction index')
    nros = [x for x in index['files'] if x['path'].lower().endswith('.nro')]
    nrrs = [x for x in index['files'] if x['path'].lower().endswith('.nrr')]
    if len(nros) != 1 or len(nrrs) != 1:
        raise ValueError('expected exactly one loose NRO and one NRR')
    image_size = image.stat().st_size
    def read_entry(source, item, limit):
        start, length = item['offset'], item['size']
        if not 0 <= start <= image_size or not 0 < length <= min(limit, image_size - start):
            raise ValueError('RomFS entry outside image')
        source.seek(start)
        data = source.read(length)
        if len(data) != length:
            raise ValueError('short read')
        return data
    with image.open('rb') as source:
        nro = read_entry(source, nros[0], 256 * 1024 * 1024)
        nrr = read_entry(source, nrrs[0], 16 * 1024 * 1024)
    if sha256(image) != index['romfs_sha256']:
        raise ValueError('RomFS image changed during preparation')
    digest = hashlib.sha256(nro).digest()
    if nrr_hashes(nrr) != {digest}:
        raise ValueError('NRR registrations do not match the loose NRO')
    synthetic = nso_from_nro(nro)
    out.mkdir(parents=True)
    (out / 'module.nro').write_bytes(nro)
    (out / 'synthetic.nso').write_bytes(synthetic)
    (out / 'manifest.json').write_text(json.dumps({
        'status': 'pass', 'romfs_sha256': index['romfs_sha256'],
        'nro_sha256': digest.hex(), 'synthetic_nso_sha256': hashlib.sha256(synthetic).hexdigest(),
        'integrity': 'NRO hash matches the NRR; NRR signature not checked; synthetic NSO is for offline rewrite only'
    }, indent=2) + '\n')
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    print(f'prepared {prepare(args.romfs.resolve(), args.out.resolve())} registered NRO for offline analysis')


if __name__ == '__main__':
    main()
