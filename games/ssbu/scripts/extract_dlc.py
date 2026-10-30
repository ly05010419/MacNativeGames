#!/usr/bin/env python3
"""Super Smash Bros. Ultimate's add-on content (Q96): which DLC the player owns, read from their own DLC NSPs.

    extract_dlc.py --keys prod.keys --dlc <folder of DLC NSPs> --output <work>/build/dlc

Each Smash DLC NSP holds a Meta NCA and a data NCA whose RomFS carries one small marker file (25.10.2026, 99 NSPs:
dummy.txt for the fighters, spirit boards and bundles, VENOM for Mii costumes); the fighters and costumes themselves
ship in the 13.0.2 update. So the runtime needs only the owned indices (nn::aoc::CountAddOnContent / ListAddOnContent,
runtime/services.c), and each marker is kept below <index>/ in case the game mounts one. Every RomFS is verified through
all six IVFC levels. Writes <output>/owned.txt (one decimal index per line, then the title id and the NSP's name) and
<output>/manifest.json; a failed run leaves no output. Nothing here goes into git: it is the player's own purchase list.
"""
import argparse
import hashlib
import json
import shutil
import struct
import tempfile
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import modes
from extract_exefs import decrypt, read_at, require, load_keys, container_entries, common_tickets, nca_inventory, nca_ctr_key, record_offset
from extract_romfs import index_romfs

APPLICATION_ID = 0x01006A800016E000
AOC_BASE = APPLICATION_ID + 0x1000   # an add-on content's title id is the base plus its index
AOC_MAX = 2048                       # runtime/services.c AOC_MAX


def dlc_index(title_id):
    """The add-on content index of a Smash DLC title id; anything else is refused."""
    index = title_id - AOC_BASE
    require(1 <= index < AOC_MAX, f'title {title_id:016X} is not a Super Smash Bros. Ultimate DLC')
    return index


def romfs_image(f, record, hdr, key):
    """The data NCA's RomFS, every IVFC level checked against the FS header's master hash."""
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + i * 16)
        if not start:
            continue
        fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
        require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash')
        if fs[2:5] != bytes([0, 3, 3]):
            continue
        require(fs[8:12] == b'IVFC' and struct.unpack_from('<III', fs, 12) == (0x20000, 32, 7), 'Unsupported IVFC layout')

        def read(relative, size):
            require(0 <= relative <= relative + size <= (end - start) * 512, 'Section bounds')
            absolute = start * 512 + relative; aligned = absolute & ~15; skip = absolute - aligned
            iv = fs[0x140:0x148][::-1] + (aligned // 16).to_bytes(8, 'big')
            return decrypt(key, modes.CTR(iv), read_at(f, record_offset(record) + aligned, size + skip))[skip:]

        parent = fs[0xc8:0xe8]
        for level in range(6):
            offset, size, power, reserved = struct.unpack_from('<QQII', fs, 0x18 + level * 24)
            require(12 <= power <= 24 and reserved == 0, 'IVFC block size'); block = 1 << power
            data = read(offset, size)
            for index in range((size + block - 1) // block):
                payload = data[index * block:(index + 1) * block]
                require(hashlib.sha256(payload + bytes(block - len(payload))).digest() == parent[index * 32:index * 32 + 32],
                        f'IVFC level {level} block {index} failed')
            parent = data
        return parent
    require(False, 'no RomFS section in the data NCA')


def read_dlc(nsp, keys):
    with nsp.open('rb') as f:
        _, entries = container_entries(f, nsp.stat().st_size)
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        meta = [r for r, _ in records if r['content_type'] == 1]
        data = [(r, h) for r, h in records if r['content_type'] in (4, 5)]
        require(len(meta) == 1 and len(data) == 1, f'{nsp.name}: expected one Meta and one data NCA, found {len(meta)} and {len(data)}')
        record, hdr = data[0]
        require(record['title_id'] == meta[0]['title_id'], f'{nsp.name}: Meta and data NCA name different titles')
        index = dlc_index(int(record['title_id'], 16))
        image = romfs_image(f, record, hdr, nca_ctr_key(hdr, keys, tickets))
    with tempfile.NamedTemporaryFile(suffix='.romfs') as t:
        t.write(image); t.flush()
        files = [dict(path=e['path'], data=image[e['offset']:e['offset'] + e['size']]) for e in index_romfs(Path(t.name))]
    return dict(index=index, title_id=record['title_id'], nsp=nsp.name, files=files)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--dlc', type=Path, required=True, help='the folder with your DLC NSPs')
    ap.add_argument('--output', type=Path, required=True)
    a = ap.parse_args()
    keys = load_keys(a.keys)
    nsps = sorted(a.dlc.glob('*.nsp'))
    require(nsps, f'no .nsp files in {a.dlc}')
    owned = {}
    for nsp in nsps:
        d = read_dlc(nsp, keys)
        require(d['index'] not in owned, f'two NSPs for DLC index {d["index"]}: {owned.get(d["index"], {}).get("nsp")} and {nsp.name}')
        owned[d['index']] = d
    tmp = a.output.with_name(a.output.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    try:
        write(tmp, owned, nsps, a.dlc)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise
    shutil.rmtree(a.output, ignore_errors=True)
    tmp.rename(a.output)
    print(json.dumps(dict(status='pass', count=len(owned), output=str(a.output))))


def write(tmp, owned, nsps, source):
    lines, listed = ['# Super Smash Bros. Ultimate add-on content owned (scripts/extract_dlc.py): index, title id, NSP'], []
    for index in sorted(owned):
        d = owned[index]
        for item in d['files']:
            path = tmp / str(index) / item['path']
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(item['data'])
        lines.append(f'{index} {d["title_id"]} {d["nsp"]}')
        listed.append(dict(index=index, title_id=d['title_id'], nsp=d['nsp'],
                           files=[dict(path=i['path'], size=len(i['data']), sha256=hashlib.sha256(i['data']).hexdigest()) for i in d['files']]))
    (tmp / 'owned.txt').write_text('\n'.join(lines) + '\n')
    sources = [dict(name=p.name, size=p.stat().st_size) for p in nsps]
    (tmp / 'manifest.json').write_text(json.dumps(dict(status='pass', source=str(source), sources=sources, count=len(listed), dlc=listed,
                                                       integrity='each data NCA: FS header and all six IVFC levels verified; RSA signature not checked'),
                                                  indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
