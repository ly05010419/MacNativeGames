#!/usr/bin/env python3
"""Verify an optional DLC NSP, extract its files, and discard the temporary image.

Checks the Data NCA title, FS header and six IVFC levels. RSA signatures are not
checked. Keys stay in memory. Only explicitly supplied DLC is installed.
"""
import argparse
import hashlib
import json
import tempfile
from pathlib import Path
from extract_exefs import (require, load_keys, container_entries, common_tickets,
                          nca_inventory, nca_ctr_key, record_offset, read_at)
from extract_romfs import index_romfs
from merge_romfs import BASE_TITLE, Ctr, romfs_section, verify_ivfc


def allowed_titles():
    base = int(BASE_TITLE, 16) + 0x1000
    return {f'{base + i:016X}': i for i in range(1, 8 if BASE_TITLE == '01006C100EC08000' else 2)}


def extract(package, keys_path, output):
    require(not output.exists(), 'Output already exists')
    keys = load_keys(keys_path)
    with package.open('rb') as source, tempfile.TemporaryDirectory(prefix='mng-dlc-') as temp:
        kind, entries = container_entries(source, package.stat().st_size)
        require(kind == 'nsp', 'DLC must be an NSP')
        data = [(r, h) for r, h in nca_inventory(source, entries, keys) if r['content_type'] == 5]
        require(len(data) == 1, 'Expected one Public Data NCA')
        record, header = data[0]
        title = record['title_id']
        require(title in allowed_titles(), f'Unsupported DLC title {title} for {BASE_TITLE}')
        start, end, fs = romfs_section(header)
        require(fs[4] == 3, 'Expected AES-CTR DLC section')
        ctr = Ctr(source, record_offset(record), record['size'],
                  nca_ctr_key(header, keys, common_tickets(source, entries)))
        def read(offset, size):
            require(0 <= offset <= offset + size <= end - start, 'DLC section bounds')
            return ctr.read(start + offset, size, fs[0x140:0x148][::-1])
        image = Path(temp) / 'image.tmp'
        counts, size, digest = verify_ivfc(fs, end - start, read, image)
        files = index_romfs(image)
        folder = output / str(allowed_titles()[title])
        root = folder / 'root'
        root.mkdir(parents=True)
        with image.open('rb') as stream:
            for item in files:
                path = root / item['path']
                require(path.resolve().is_relative_to(root.resolve()), 'Unsafe DLC path')
                path.parent.mkdir(parents=True, exist_ok=True)
                hasher = hashlib.sha256()
                with path.open('xb') as dest:
                    for pos in range(0, item['size'], 8 << 20):
                        chunk = read_at(stream, item['offset'] + pos, min(8 << 20, item['size'] - pos))
                        dest.write(chunk)
                        hasher.update(chunk)
                item['sha256'] = hasher.hexdigest()
        report = dict(status='pass', title_id=title, index=allowed_titles()[title],
                      romfs_size=size, romfs_sha256=digest, files=files, ivfc_blocks=counts,
                      integrity='FS header and six IVFC levels verified; RSA signature not checked')
        (folder / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dlc', type=Path)
    parser.add_argument('--keys', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = extract(args.dlc, args.keys, args.output)
    print(json.dumps({k: report[k] for k in ('status', 'title_id', 'index', 'romfs_size', 'romfs_sha256')}))


if __name__ == '__main__':
    main()
