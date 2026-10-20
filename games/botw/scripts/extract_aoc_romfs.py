#!/usr/bin/env python3
"""Breath of the Wild: extract the add-on content (DLC) RomFS images from the game file.

The cartridge image holds the two add-on content titles beside the game: 01007EF00011F001 (index 1, the expansion pass
packs) and 01007EF00011F002 (index 2), each one PublicData NCA with one RomFS section, key-area encrypted (no title
keys). The game lists both and mounts index 1 as "aoc". Each NCA is copied out of the game file and its RomFS section
extracted with the pinned hactool, which verifies every IVFC level; the result is indexed like the game's RomFS
(index.json, and runtime-index.bin for the runtime). Keys stay in memory or in a private temporary file; nothing prints
them.
usage: extract_aoc_romfs.py <game file> --keys <prod.keys> --hactool <pinned hactool> --output <new folder>
The output folder gets one subfolder per add-on content index: <output>/1/{romfs.bin, index.json, runtime-index.bin}.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

TOTK_SCRIPTS = next(p for p in (Path(__file__).resolve().parents[2] / 'totk/scripts', Path(__file__).resolve().parents[2] / 'scripts')
                    if (p / 'extract_exefs.py').exists())
sys.path.insert(0, str(TOTK_SCRIPTS))
from extract_exefs import load_keys, container_entries, nca_inventory, record_offset, require  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402
from extract_update_romfs import copy_range, hactool_prod_keys  # noqa: E402

AOC = {'01007EF00011F001': 1, '01007EF00011F002': 2}


def aoc_ncas(game, keys):
    with game.open('rb') as f:
        _, entries = container_entries(f, game.stat().st_size)
        records = nca_inventory(f, entries, keys)
    found = {}
    for r, h in records:
        if r['title_id'] in AOC and r['content_type'] == 5:   # PublicData
            require(h[0x230:0x240] == bytes(16), 'Rights-managed add-on content is not supported here')
            sections = [i for i in range(4) if h[0x240 + i * 16:0x240 + i * 16 + 4] != bytes(4)]
            require(sections == [0] and h[0x400 + 2] == 0 and h[0x400 + 3] == 3, 'Expected one RomFS section with an IVFC hash tree')   # fs type RomFS, IVFC
            require(AOC[r['title_id']] not in found, 'Two data NCAs for one add-on content title')
            found[AOC[r['title_id']]] = r
    require(sorted(found) == [1, 2], f'Expected add-on content 1 and 2 in the game file, found {sorted(found)}')
    return found


def runtime_index(files):
    strings = bytearray(); rows = bytearray()
    for item in files:
        name = item['path'].encode() + b'\0'
        rows.extend(struct.pack('<QQII', item['offset'], item['size'], len(strings), len(name))); strings.extend(name)
    return struct.pack('<8sQQ', b'TOTKRFS1', len(files), 24 + len(rows)) + rows + strings


def extract_one(game, record, index, hactool, keys_path, output):
    with tempfile.TemporaryDirectory(prefix=f'aoc{index}-', dir=output) as tmp:
        tmp = Path(tmp)
        copy_range(game, record_offset(record), record['size'], tmp / 'aoc.nca')
        hactool_keys = tmp / 'hactool.keys'
        hactool_prod_keys(keys_path, hactool_keys)
        result = tmp / 'result'; result.mkdir()
        command = [str(hactool.resolve()), '-x', '-y', '--disablekeywarns', '--suppresskeys', '-k', str(hactool_keys.resolve()),
                   '--section0=' + str(result / 'romfs.bin'), str(tmp / 'aoc.nca')]
        with (result / 'extract.log').open('w') as log:
            code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT).returncode
        text = (result / 'extract.log').read_text()
        # As for the update: NCAs converted into a cartridge image lose their RSA signatures; the hash tree still verifies.
        unsigned = re.findall(r'^\s*(Fixed-Key Signature|NPDM Signature) \(FAIL\)', text, re.M)
        bad = re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', re.sub(r'^\s*(Fixed-Key Signature|NPDM Signature) \(FAIL\).*$', '', text, flags=re.M))
        if code or bad:
            (output / f'{index}-failure.log').write_text(re.sub(r'[0-9A-Fa-f]{32,}', '<hex removed>', text))
        require(code == 0 and not bad, f'add-on content {index}: hactool reported a failure')
        levels = sorted(set(int(m) for m in re.findall(r'Level (\d) \(GOOD\)', text)))
        require(levels and levels == list(range(levels[-1] + 1)), 'Missing verified IVFC levels')
        image = result / 'romfs.bin'
        with image.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        files = index_romfs(image)
        (result / 'runtime-index.bin').write_bytes(runtime_index(files))
        report = dict(status='pass', title_id=next(t for t, i in AOC.items() if i == index), index=index, nca=record['name'],
                      romfs_size=image.stat().st_size, romfs_sha256=digest, files=files,
                      integrity=f'IVFC levels 0-{levels[-1]} verified by hactool', unverified_signatures=sorted(set(unsigned)))
        (result / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        result.rename(output / str(index))
        return dict(index=index, files=len(files), size=report['romfs_size'], sha256=digest)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', required=True, type=Path)
    ap.add_argument('--hactool', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    require(not a.output.exists(), 'Output exists')
    found = aoc_ncas(a.game, load_keys(a.keys))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(a.output.parent).free > 2 * sum(r['size'] for r in found.values()) + (1 << 30), 'Not enough free space')
    a.output.mkdir()
    done = [extract_one(a.game, found[i], i, a.hactool, a.keys, a.output) for i in sorted(found)]
    print(json.dumps(dict(status='pass', packs=done)))


if __name__ == '__main__':
    main()
