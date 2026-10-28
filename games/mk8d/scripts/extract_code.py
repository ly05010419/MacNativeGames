#!/usr/bin/env python3
"""Extract and verify Mario Kart 8 Deluxe ExeFS from a local XCI/NSP."""
import argparse
import hashlib
import importlib.util
import io
import json
import struct
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import modes

from mk8d import TITLE

GENERIC = Path(__file__).resolve().parents[1] / 'third_party/totk-scripts/extract_exefs.py'
spec = importlib.util.spec_from_file_location('generic_exefs', GENERIC)
ex = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ex)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--program', choices=('base', 'update'), default='update')
    args = parser.parse_args()
    ex.require(not args.output.exists(), 'Output already exists')
    keys = ex.load_keys(args.keys)
    with args.game.open('rb') as f:
        kind, entries = ex.container_entries(f, args.game.stat().st_size)
        tickets = ex.common_tickets(f, entries)
        records = ex.nca_inventory(f, entries, keys)
        programs = []
        for record, header in records:
            if record['content_type'] != 0 or record['title_id'] != TITLE:
                continue
            is_update = any(header[0x400 + i * 512 + 4] == 4 for i in range(4))
            if is_update == (args.program == 'update'):
                programs.append((record, header))
        ex.require(len(programs) == 1, f'Expected exactly one Mario Kart 8 Deluxe {args.program} Program NCA')
        record, header = programs[0]
        key = ex.nca_ctr_key(header, keys, tickets)
        for i in range(4):
            start, end = struct.unpack_from('<II', header, 0x240 + 16 * i)
            if not start:
                continue
            fs = header[0x400 + i * 512:0x600 + i * 512]
            ex.require(hashlib.sha256(fs).digest() == header[0x280 + 32*i:0x2a0 + 32*i], 'FS header hash mismatch')
            if fs[2:4] != b'\x01\x02':
                continue
            ex.require(fs[4] == 3, 'Unsupported ExeFS encryption')
            blocksize, levels = struct.unpack_from('<II', fs, 0x28)
            hashoff, hashsize, pfsoff, pfssize = struct.unpack_from('<QQQQ', fs, 0x30)
            secstart, secsize = start * 512, (end - start) * 512
            ex.require(levels == 2 and blocksize > 0 and pfssize <= 512*1024*1024, 'Unsupported ExeFS layout')

            def section_read(relative, size):
                ex.require(relative >= 0 and relative + size <= secsize, 'Section read out of bounds')
                absolute = secstart + relative
                aligned = absolute & ~15
                iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
                data = ex.read_at(f, record['nca_offset'] + aligned, size + absolute - aligned)
                return ex.decrypt(key, modes.CTR(iv), data)[absolute - aligned:]

            hashes = section_read(hashoff, hashsize)
            ex.require(hashlib.sha256(hashes).digest() == fs[8:40], 'ExeFS hash table mismatch')
            pfs = section_read(pfsoff, pfssize)
            for index, pos in enumerate(range(0, pfssize, blocksize)):
                ex.require(hashlib.sha256(pfs[pos:pos+blocksize]).digest() == hashes[index*32:(index+1)*32],
                           f'ExeFS block hash mismatch: {index}')
            files = [(name, pfs[offset:offset+size]) for name, offset, size in
                     ex.partition_entries(io.BytesIO(pfs), 0, len(pfs))]
            ex.require(any(name == 'main.npdm' for name, _ in files), 'Partition is not ExeFS')
            args.output.mkdir(parents=True)
            extracted = []
            for name, data in files:
                (args.output / name).write_bytes(data)
                extracted.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            manifest = {'container': kind, 'title_id': TITLE, 'program': args.program,
                        'program_nca_size': record['size'], 'section_index': i,
                        'integrity': 'FS header, hash table and every ExeFS block verified; RSA signatures not checked',
                        'files': extracted}
            (args.output / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(json.dumps(manifest, indent=2))
            return
        raise ValueError('No ExeFS section found')


if __name__ == '__main__':
    main()
