#!/usr/bin/env python3
"""Breath of the Wild: extract and verify the 1.6.0 ExeFS from the game file.

The game file holds the base game, the 1.6.0 update and the add-on content (a cartridge image made that way). The code
comes from the update's Program NCA (the one with a BKTR section, key-area encrypted, title 01007EF00011E000): its ExeFS
section is read in place, the FS header hash, the hash table and every data block are verified, and the files are
written with an extraction-manifest.json in the format of games/totk/scripts/extract_exefs.py (build_app.py reads it).
usage: extract_exefs.py <game file> --keys <prod.keys> --output <new folder>
"""
import argparse
import hashlib
import io
import json
import struct
import sys
from pathlib import Path

TOTK_SCRIPTS = next(p for p in (Path(__file__).resolve().parents[2] / 'totk/scripts', Path(__file__).resolve().parents[2] / 'scripts')
                    if (p / 'extract_exefs.py').exists())
sys.path.insert(0, str(TOTK_SCRIPTS))
from extract_exefs import (container_entries, decrypt, load_keys, nca_ctr_key, nca_inventory, partition_entries,  # noqa: E402
                           read_at, record_offset, require)
from cryptography.hazmat.primitives.ciphers import modes  # noqa: E402

TITLE = '01007EF00011E000'


def update_program(records):
    bktr = lambda h: any(h[0x400 + i * 512 + 4] == 4 for i in range(4))
    found = [(r, h) for r, h in records if r['content_type'] == 0 and r['title_id'] == TITLE and bktr(h)]
    require(len(found) == 1, f'Expected one update Program NCA of {TITLE} (the one with a BKTR section)')
    require(found[0][1][0x230:0x240] == bytes(16), 'Rights-managed NCAs are not supported here')
    return found[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    require(not a.output.exists(), 'Output already exists; choose a new directory')
    keys = load_keys(a.keys)
    with a.game.open('rb') as f:
        kind, entries = container_entries(f, a.game.stat().st_size)
        records = nca_inventory(f, entries, keys)
        record, hdr = update_program(records)
        ctr_key = nca_ctr_key(hdr, keys, {})
        for i in range(4):
            start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
            if not start:
                continue
            fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
            require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2A0 + 32 * i], 'FS header hash mismatch')
            if fs[2:4] != b'\x01\x02':
                continue
            require(fs[4] == 3, 'Only AES-CTR ExeFS is supported')
            blocksize, levels = struct.unpack_from('<II', fs, 0x28)
            hashoff, hashsize, pfsoff, pfssize = struct.unpack_from('<QQQQ', fs, 0x30)
            secstart, secsize = start * 512, (end - start) * 512
            require(levels == 2 and blocksize > 0 and pfssize <= 512 * 1024 * 1024, 'Unsupported ExeFS hash layout')

            def section_read(relative, size):
                require(relative >= 0 and relative + size <= secsize, 'Section read out of bounds')
                absolute = secstart + relative
                aligned = absolute & ~15
                iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
                data = read_at(f, record_offset(record) + aligned, size + absolute - aligned)
                return decrypt(ctr_key, modes.CTR(iv), data)[absolute - aligned:]

            hashes = section_read(hashoff, hashsize)
            require(hashlib.sha256(hashes).digest() == fs[8:40], 'ExeFS hash-table mismatch')
            pfs = section_read(pfsoff, pfssize)
            for index, pos in enumerate(range(0, pfssize, blocksize)):
                require(hashlib.sha256(pfs[pos:pos + blocksize]).digest() == hashes[index * 32:(index + 1) * 32],
                        f'ExeFS block hash mismatch: {index}')
            files = [(name, pfs[off:off + size]) for name, off, size in partition_entries(io.BytesIO(pfs), 0, len(pfs))]
            require(any(name == 'main.npdm' for name, _ in files), 'PFS0 is not ExeFS')
            a.output.mkdir(parents=True)
            extracted = []
            for name, data in files:
                (a.output / name).write_bytes(data)
                extracted.append({'name': name, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
            manifest = {'source': str(a.game.resolve()), 'source_size': a.game.stat().st_size,
                        'source_mtime_ns': a.game.stat().st_mtime_ns, 'container': kind,
                        'nca_inventory': [r for r, _ in records], 'program': record, 'section_index': i,
                        'integrity': 'FS header, ExeFS hash table, and every ExeFS data block verified; RSA signatures not checked',
                        'files': extracted}
            (a.output.parent / 'extraction-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
            print(json.dumps({'integrity': manifest['integrity'], 'files': extracted}, indent=2))
            return
        raise ValueError('No ExeFS section found')


if __name__ == '__main__':
    main()
