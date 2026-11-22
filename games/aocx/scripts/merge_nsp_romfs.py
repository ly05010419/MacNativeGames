#!/usr/bin/env python3
"""Merge an update NSP's RomFS (BKTR) over the base NSP's RomFS in one pass, reading the base straight from its NSP.

    python3 scripts/merge_nsp_romfs.py <base.nsp> <update.nsp> --keys prod.keys --output <work>/extracted/local/romfs

Hyrule Warriors: Age of Calamity comes as two files: the base game (1.0.0, an 11.4 GB NSP) and the 1.4.1 update (a
patch NSP). Red Dead Redemption's merge (games/rdr/scripts/merge_card_romfs.py) reads both Program NCAs from one card
image; this one takes the base Program NCA from the base NSP and the update's patch NCA from the update NSP, and
otherwise reuses that merge as it is: games/pla's bucket trees, indirect and AES-CTR-Ex reads and the six-level IVFC
check of the merged image, and rdr's `CardBase` (the base RomFS section decrypted on demand, here from the base NSP).

Integrity: every block of the merged image is checked against the update's IVFC hash tree (all six levels), which covers
the bytes taken from the base too; RSA signatures are not checked (as the other extraction scripts). Keys stay in memory.
Writes romfs.bin and index.json (games/pla's report shape, which scripts/prepare_runtime.py reads).
"""
import argparse
import json
import sys
from pathlib import Path

GAMES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAMES / 'pla/scripts'))
sys.path.insert(0, str(GAMES / 'rdr/scripts'))
from pla_artifact_paths import outside_source  # noqa: E402
from extract_exefs import (require, load_keys, container_entries, common_tickets, nca_inventory,  # noqa: E402
                           program_nca, nca_ctr_key)
from extract_romfs import index_romfs  # noqa: E402
from merge_romfs import Ctr, Patched, verify_ivfc  # noqa: E402
from merge_card_romfs import CardBase, romfs_section  # noqa: E402

TITLE = '01002B00111A2000'
UPDATE_TITLE = '01002B00111A2800'


def open_program(f, path, keys, update, title, update_title):
    """(record, header, Ctr) of the base or the update Program NCA of one NSP."""
    _, entries = container_entries(f, path.stat().st_size)
    tickets = common_tickets(f, entries)
    records = nca_inventory(f, entries, keys)
    rec, hdr = program_nca(records, update=update, title=title, update_title=update_title)
    return rec, hdr, Ctr(f, rec['nca_offset'], rec['size'], nca_ctr_key(hdr, keys, tickets))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('base', type=Path, help='the base game NSP (its Program NCA holds the full RomFS)')
    parser.add_argument('update', type=Path, help='the update NSP (its Program NCA holds the BKTR patch)')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title', default=TITLE)
    parser.add_argument('--update-title', default=UPDATE_TITLE)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output = outside_source(args.output)
    require(not (args.output / 'romfs.bin').exists(), 'romfs.bin already exists; choose a new directory')
    keys = load_keys(args.keys)
    with args.base.open('rb') as bf, args.update.open('rb') as uf:
        base_rec, base_hdr, bctr = open_program(bf, args.base, keys, False, args.title, args.update_title)
        upd_rec, upd_hdr, uctr = open_program(uf, args.update, keys, True, args.title, args.update_title)
        bstart, bend, bfs = romfs_section(base_hdr)
        ustart, uend, ufs = romfs_section(upd_hdr)
        patched = Patched(uctr, ustart, uend, ufs, CardBase(bctr, bstart, bend - bstart, bfs))
        args.output.mkdir(parents=True, exist_ok=True)
        image = args.output / 'romfs.bin'
        counts, size, sha = verify_ivfc(ufs, patched.total, patched.read, image)
    files = index_romfs(image)
    report = dict(status='pass', base=dict(source=str(args.base.resolve()), source_size=args.base.stat().st_size),
                  update=dict(source=str(args.update.resolve()), source_size=args.update.stat().st_size),
                  base_program=base_rec, update_program=upd_rec, ivfc_blocks=counts, romfs_size=size,
                  romfs_sha256=sha, bytes_from_base=patched.from_base, bytes_from_update=patched.from_update,
                  files=files, integrity='FS headers and all six IVFC levels of the merged image verified (the base '
                  'bytes read from the base NSP); RSA signatures not checked')
    (args.output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status='pass', file_count=len(files), size=size, sha256=sha,
                          bytes_from_base=patched.from_base, bytes_from_update=patched.from_update)), flush=True)


if __name__ == '__main__':
    main()
