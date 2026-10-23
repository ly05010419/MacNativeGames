#!/usr/bin/env python3
"""Extract and verify the Program ExeFS of a Bayonetta 3 update NSP.

The base NSP's ExeFS is sparse and empty (every module ships in the update), so the code always comes from the update.
FS header, ExeFS hash table and every ExeFS block are verified. Keys stay in memory. No executable launch, no network.
"""
import argparse
import hashlib
import io
import json
import struct
from pathlib import Path
from merge_romfs import BASE_TITLE, open_program, require, load_keys   # also puts this folder on sys.path
from extract_exefs import partition_entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('update', type=Path, help='update NSP')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title', default=BASE_TITLE, help='program title ID (default: Bayonetta 3, program 0)')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists; choose a new directory')
    f, record, hdr, ctr = open_program(args.update, load_keys(args.keys), args.title)
    with f:
        for i in range(4):
            start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
            fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
            if not start or fs[2:4] != b'\x01\x02':
                continue
            require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
            require(fs[4] == 3, 'Only AES-CTR ExeFS is supported')
            blocksize, levels = struct.unpack_from('<II', fs, 0x28)
            hashoff, hashsize, pfsoff, pfssize = struct.unpack_from('<QQQQ', fs, 0x30)
            require(levels == 2 and blocksize > 0 and pfssize <= 512 * 1024 * 1024, 'Unsupported ExeFS hash layout')
            require(hashoff + hashsize <= (end - start) * 512 and pfsoff + pfssize <= (end - start) * 512, 'ExeFS out of bounds')
            upper = fs[0x140:0x148][::-1]
            hashes = ctr.read(start * 512 + hashoff, hashsize, upper)
            require(hashlib.sha256(hashes).digest() == fs[8:40], 'ExeFS hash-table mismatch')
            pfs = ctr.read(start * 512 + pfsoff, pfssize, upper)
            for index, pos in enumerate(range(0, pfssize, blocksize)):
                require(hashlib.sha256(pfs[pos:pos + blocksize]).digest() == hashes[index * 32:(index + 1) * 32],
                        f'ExeFS block hash mismatch: {index}')
            files = [(name, pfs[off:off + size]) for name, off, size in partition_entries(io.BytesIO(pfs), 0, len(pfs))]
            require(any(name == 'main.npdm' for name, _ in files), 'PFS0 is not ExeFS')
            args.output.mkdir(parents=True)
            extracted = []
            for name, data in files:
                (args.output / name).write_bytes(data)
                extracted.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            stat = args.update.stat()
            manifest = {'source': str(args.update.resolve()), 'source_size': stat.st_size, 'source_mtime_ns': stat.st_mtime_ns,
                        'container': 'nsp', 'program': record, 'section_index': i,
                        'integrity': 'FS header, ExeFS hash table, and every ExeFS data block verified; RSA signatures not checked',
                        'files': extracted}
            (args.output.parent / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(json.dumps({'integrity': manifest['integrity'], 'files': extracted}, indent=2))
            return
    raise ValueError('No ExeFS section found')


if __name__ == '__main__':
    main()
