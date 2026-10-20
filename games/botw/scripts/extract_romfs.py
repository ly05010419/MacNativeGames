#!/usr/bin/env python3
"""Breath of the Wild: merge the base game's RomFS and an update's BKTR patch from one game file into a RomFS image.

The game file may hold the base game, the update and DLC together (an XCI made that way): the base Program NCA is the
one without a BKTR section, the update's is the one with it, both with the game's title ID. The pinned hactool reads the
base NCA in place (its bounded base-file hook), so only the update NCA is copied. Keys stay in memory or in private
temporary files; nothing prints them. All six IVFC levels of the result are verified and its files indexed.
usage: extract_romfs.py <game file> --keys <prod.keys> --hactool <pinned hactool> --output <new folder>
"""
import argparse
import hashlib
import json
import os
import re
import shutil
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

TITLE = '01007EF00011E000'


def programs(game, keys):
    with game.open('rb') as f:
        _, entries = container_entries(f, game.stat().st_size)
        records = nca_inventory(f, entries, keys)
    found = [(r, h) for r, h in records if r['content_type'] == 0 and r['title_id'] == TITLE]
    bktr = lambda h: [i for i in range(4) if h[0x400 + i * 512 + 4] == 4]
    base = [(r, h) for r, h in found if not bktr(h)]
    update = [(r, h) for r, h in found if bktr(h)]
    require(len(base) == 1 and len(update) == 1, f'Expected one base and one update Program NCA of {TITLE}')
    require(len(bktr(update[0][1])) == 1, 'Expected one BKTR section')
    for _, h in base + update:
        require(h[0x230:0x240] == bytes(16), 'Rights-managed NCAs are not supported here')
    return base[0][0], update[0][0], bktr(update[0][1])[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', required=True, type=Path)
    ap.add_argument('--hactool', required=True, type=Path)
    ap.add_argument('--output', required=True, type=Path)
    a = ap.parse_args()
    require(not a.output.exists(), 'Output exists')
    base, update, section = programs(a.game, load_keys(a.keys))
    a.output.parent.mkdir(parents=True, exist_ok=True)
    require(shutil.disk_usage(a.output.parent).free > update['size'] + 16 * (1 << 30), 'Need about 17 GiB free')
    with tempfile.TemporaryDirectory(prefix='update-merge-', dir=a.output.parent) as tmp:
        tmp = Path(tmp)
        keyfile = tmp / 'title.keys'
        os.close(os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600))   # no rights-managed NCA: no title keys
        copy_range(a.game, record_offset(update), update['size'], tmp / 'update.nca')
        result = tmp / 'result'
        result.mkdir()
        hactool_keys = tmp / 'hactool.keys'
        hactool_prod_keys(a.keys, hactool_keys)
        env = dict(os.environ, MNG_TITLE_KEYS=str(keyfile), MNG_BASE_RANGE_OFFSET=str(record_offset(base)), MNG_BASE_RANGE_SIZE=str(base['size']))
        command = [str(a.hactool.resolve()), '-x', '-y', '--disablekeywarns', '--suppresskeys', '-k', str(hactool_keys.resolve()),
                   '--basenca=' + str(a.game.resolve()), f'--section{section}=' + str(result / 'romfs.bin'), str(tmp / 'update.nca')]
        with (result / 'merge.log').open('w') as log:
            code = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
        text = (result / 'merge.log').read_text()
        # A game file that holds the update and DLC in an XCI had their NCAs converted from title-key crypto: their headers
        # changed, so the header and NPDM RSA signatures no longer verify. The content is still verified by its hash tree
        # (the FS headers' hashes and all six IVFC levels below); every other check stays strict.
        unsigned = re.findall(r'^\s*(Fixed-Key Signature|NPDM Signature) \(FAIL\)', text, re.M)
        bad = re.search(r'\(FAIL\)|\(INVALID\)|Failed to|Error:', re.sub(r'^\s*(Fixed-Key Signature|NPDM Signature) \(FAIL\).*$', '', text, flags=re.M))
        if code or bad:
            (a.output.parent / (a.output.name + '-failure.log')).write_text(re.sub(r'[0-9A-Fa-f]{32,}', '<hex removed>', text))
        require(code == 0 and not bad, 'BKTR verification failed')
        require(all(re.search(rf'Level {i} \(GOOD\)', text) for i in range(6)), 'Missing verified IVFC levels')
        image = result / 'romfs.bin'
        with image.open('rb') as f:
            digest = hashlib.file_digest(f, 'sha256').hexdigest()
        files = index_romfs(image)
        report = dict(status='pass', title_id=TITLE, base_nca=base['name'], update_nca=update['name'], romfs_size=image.stat().st_size,
                      romfs_sha256=digest, files=files, integrity='BKTR merge; all six IVFC levels verified',
                      unverified_signatures=sorted(set(unsigned)))
        (result / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        result.rename(a.output)
    print(json.dumps(dict(status='pass', files=len(files), size=report['romfs_size'], sha256=digest)))


if __name__ == '__main__':
    main()
