#!/usr/bin/env python3
"""校验玩家提供的圣兽之王 DLC 授权包，并提取其只读资源镜像。"""
import argparse
import hashlib
import json
from pathlib import Path

from extract_exefs import (load_keys, container_entries, common_tickets, nca_inventory,
                           nca_ctr_key, require)
from extract_romfs import index_romfs
from merge_romfs import Ctr, romfs_section, verify_ivfc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('packages', nargs='+', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists')
    keys = load_keys(args.keys)
    seen = set()
    for package in args.packages:
        with package.open('rb') as source:
            _, entries = container_entries(source, package.stat().st_size)
            records = nca_inventory(source, entries, keys)
            data = [(r, h) for r, h in records if r['content_type'] == 5]
            require(len(data) == 1, 'Expected one PublicData NCA')
            record, header = data[0]
            title = int(record['title_id'], 16)
            index = title - 0x010054B01AD93000
            require(index in (1, 2) and index not in seen, 'Unexpected or duplicate DLC title')
            require(all(int(r['title_id'], 16) == title for r, _ in records), 'Mixed-title DLC')
            seen.add(index)
            key = nca_ctr_key(header, keys, common_tickets(source, entries))
            start, end, fs = romfs_section(header)
            require(fs[4] == 3, 'Expected AES-CTR DLC')
            ctr = Ctr(source, record['nca_offset'], record['size'], key)
            upper = fs[0x140:0x148][::-1]
            folder = args.output / str(index)
            folder.mkdir(parents=True)
            image = folder / 'romfs.bin'
            blocks, size, digest = verify_ivfc(fs, end - start,
                lambda offset, length: ctr.read(start + offset, length, upper), image)
            files = index_romfs(image)
            report = dict(status='pass', index=index, title_id=record['title_id'],
                          source=str(package.resolve()), source_sha256=hashlib.sha256(package.read_bytes()).hexdigest(),
                          romfs_size=size, romfs_sha256=digest, ivfc_blocks=blocks, files=files,
                          integrity='FS header and all six IVFC levels verified; RSA signatures not checked')
            (folder / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report))


if __name__ == '__main__':
    main()
