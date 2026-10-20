#!/usr/bin/env python3
"""Which game a game file holds: the title IDs of its NCAs, with content types and whether a Program NCA carries a BKTR
update section. build_app.py chooses the game from this, never from the file name. Nothing from the keys is printed.
usage: identify_game.py <game.xci|game.nsp> --keys <prod.keys>"""
import argparse
import json
from pathlib import Path

from extract_exefs import container_entries, load_keys, nca_inventory


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game', type=Path)
    ap.add_argument('--keys', required=True, type=Path)
    a = ap.parse_args()
    keys = load_keys(a.keys)
    with a.game.open('rb') as f:
        kind, entries = container_entries(f, a.game.stat().st_size)
        records = nca_inventory(f, entries, keys)
    ncas = [dict(title_id=r['title_id'], content_type=r['content_type'], size=r['size'],
                 bktr=any(h[0x400 + i * 512 + 4] == 4 for i in range(4))) for r, h in records]
    print(json.dumps(dict(kind=kind, titles=sorted({n['title_id'] for n in ncas}), ncas=ncas)))


if __name__ == '__main__':
    main()
