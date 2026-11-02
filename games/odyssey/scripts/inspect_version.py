#!/usr/bin/env python3
"""Verify Odyssey Control RomFS images and report their displayed NACP versions.

Keys remain in memory; extracted control data lives only in a temporary ignored
build directory. The report contains metadata and hashes, not game files.
"""
import argparse
import importlib.util
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
# 1.4.1 has 1.3.0's executables, and its RomFS merged with the base is 1.3.0's merged image, byte for byte (the same
# size and SHA-256 as assets/expected-local-update.json); only the update's Control data names the later version.
UPDATE_VERSIONS = ('1.3.0', '1.4.1')
sys.path.insert(0, str(HERE / 'third_party/by3-scripts'))
from extract_exefs import (load_keys, container_entries, common_tickets,
                           nca_inventory, nca_ctr_key, require)
from extract_romfs import index_romfs

spec = importlib.util.spec_from_file_location('by3_merge_romfs', HERE / 'third_party/by3-scripts/merge_romfs.py')
by3_merge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(by3_merge)
Ctr, romfs_section, verify_ivfc = by3_merge.Ctr, by3_merge.romfs_section, by3_merge.verify_ivfc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=HERE / 'Odyssey_build/analysis/versions.json')
    args = parser.parse_args()
    keys = load_keys(args.keys)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    versions = []
    with args.game.open('rb') as source:
        _, entries = container_entries(source, args.game.stat().st_size)
        tickets = common_tickets(source, entries)
        controls = [(record, hdr) for record, hdr in nca_inventory(source, entries, keys)
                    if record['content_type'] == 2 and record['title_id'] == '0100000000010000']
        require(len(controls) == 2, 'Expected base and update Control NCAs')
        for record, hdr in controls:
            start, end, fs = romfs_section(hdr)
            require(fs[4] == 3, 'Expected direct AES-CTR Control RomFS')
            ctr = Ctr(source, record['nca_offset'], record['size'], nca_ctr_key(hdr, keys, tickets))
            require(end <= ctr.size, 'Control RomFS exceeds stored NCA')
            upper = fs[0x140:0x148][::-1]
            with tempfile.TemporaryDirectory(dir=HERE / 'Odyssey_build') as temporary:
                image = Path(temporary) / 'control-romfs.bin'
                counts, size, digest = verify_ivfc(fs, end - start,
                    lambda offset, length: ctr.read(start + offset, length, upper), image)
                files = index_romfs(image)
                nacp = [item for item in files if item['path'] == 'control.nacp']
                require(len(nacp) == 1 and nacp[0]['size'] >= 0x3070, 'Expected control.nacp')
                with image.open('rb') as data:
                    data.seek(nacp[0]['offset'] + 0x3060)
                    raw = data.read(16)
                version = raw.split(b'\0', 1)[0].decode('ascii')
                require(version in ('1.0.0', *UPDATE_VERSIONS), 'Unexpected Odyssey display version')
                versions.append(dict(nca=record['name'], display_version=version,
                                     romfs_size=size, romfs_sha256=digest, ivfc_blocks=counts))
    require(sorted(item['display_version'] for item in versions) in (['1.0.0', v] for v in UPDATE_VERSIONS),
            'Base and update versions do not match the expected pair')
    report = dict(status='pass', versions=versions,
                  integrity='Both Control FS headers and all six IVFC levels verified; RSA signatures not checked')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({item['nca']: item['display_version'] for item in versions}))


if __name__ == '__main__':
    main()
