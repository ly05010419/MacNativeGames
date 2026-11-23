#!/usr/bin/env python3
"""Merge a card image's update RomFS (BKTR) over its base RomFS in one pass, reading the base straight from the card.

    python3 scripts/merge_card_romfs.py <game.xci> --keys prod.keys --output <work>/extracted/local/romfs

Red Dead Redemption's card ("1G+1U") holds the 12 GB base Program NCA and the 1.0.4 update's patch NCA. The Zelda line's
merge (games/pla/scripts/merge_romfs.py) reads the base from an already extracted base RomFS image; for this game that
image would be another 11 GB on a disk that has no room for it. So this script reuses that merge as it is (its bucket
trees, indirect and AES-CTR-Ex reads, and its six-level IVFC check of the merged image) and only replaces the base
image by `CardBase`, which decrypts the base RomFS section from the card on demand.

Integrity: every block of the merged image is checked against the update's IVFC hash tree (all six levels), which covers
the bytes taken from the base too; RSA signatures are not checked (as the other extraction scripts). Keys stay in memory.
Writes romfs.bin and index.json (games/pla's report shape, which scripts/prepare_runtime.py reads).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

PLA_SCRIPTS = Path(__file__).resolve().parents[2] / 'pla/scripts'
sys.path.insert(0, str(PLA_SCRIPTS))
from pla_artifact_paths import outside_source  # noqa: E402
from extract_exefs import (require, load_keys, container_entries, common_tickets, nca_inventory,  # noqa: E402
                           program_nca, nca_ctr_key)
from extract_romfs import index_romfs  # noqa: E402
from merge_romfs import Ctr, Patched, verify_ivfc  # noqa: E402

TITLE = '01007820196A6000'
UPDATE_TITLE = '01007820196A6800'


class CardBase:
    """The base Program NCA's RomFS section, decrypted from the card at the section's virtual offsets (what the
    update's indirect table addresses as storage 0: the section's hash levels and its RomFS data alike)."""

    def __init__(self, ctr, section_start, section_size, fs):
        require(fs[3] == 3 and fs[4] == 3, 'Expected a plain AES-CTR RomFS section in the base NCA')
        self.ctr, self.start, self.size, self.upper = ctr, section_start, section_size, fs[0x140:0x148][::-1]

    def read(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'Base section read out of bounds')
        return self.ctr.read(self.start + offset, size, self.upper)


def romfs_section(hdr):
    """(start, end, fs header) of the NCA's RomFS section (hash type 3), its FS header hash checked."""
    import hashlib
    for i in range(4):
        start, end = struct.unpack_from('<II', hdr, 0x240 + 16 * i)
        fs = hdr[0x400 + 512 * i:0x600 + 512 * i]
        if start and fs[3] == 3:
            require(hashlib.sha256(fs).digest() == hdr[0x280 + 32 * i:0x2a0 + 32 * i], 'FS header hash mismatch')
            return start * 512, end * 512, fs
    raise ValueError('No RomFS section in the NCA')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('game', type=Path, help='the card image (.xci) holding the base and the update Program NCAs')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--title', default=TITLE)
    parser.add_argument('--update-title', default=UPDATE_TITLE)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    args.output = outside_source(args.output)
    require(not (args.output / 'romfs.bin').exists(), 'romfs.bin already exists; choose a new directory')
    keys = load_keys(args.keys)
    with args.game.open('rb') as f:
        _, entries = container_entries(f, args.game.stat().st_size)
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        base_rec, base_hdr = program_nca(records, title=args.title, update_title=args.update_title)
        upd_rec, upd_hdr = program_nca(records, update=True, title=args.title, update_title=args.update_title)
        bstart, bend, bfs = romfs_section(base_hdr)
        ustart, uend, ufs = romfs_section(upd_hdr)
        bctr = Ctr(f, base_rec['nca_offset'], base_rec['size'], nca_ctr_key(base_hdr, keys, tickets))
        uctr = Ctr(f, upd_rec['nca_offset'], upd_rec['size'], nca_ctr_key(upd_hdr, keys, tickets))
        patched = Patched(uctr, ustart, uend, ufs, CardBase(bctr, bstart, bend - bstart, bfs))
        args.output.mkdir(parents=True, exist_ok=True)
        image = args.output / 'romfs.bin'
        counts, size, sha = verify_ivfc(ufs, patched.total, patched.read, image)
    files = index_romfs(image)
    report = dict(status='pass', game=dict(source=str(args.game.resolve()), source_size=args.game.stat().st_size),
                  base_program=base_rec, update_program=upd_rec, ivfc_blocks=counts, romfs_size=size,
                  romfs_sha256=sha, bytes_from_base=patched.from_base, bytes_from_update=patched.from_update,
                  files=files, integrity='FS headers and all six IVFC levels of the merged image verified (the base '
                  'bytes read from the card); RSA signatures not checked')
    (args.output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(status='pass', file_count=len(files), size=size, sha256=sha,
                          bytes_from_base=patched.from_base, bytes_from_update=patched.from_update)), flush=True)


if __name__ == '__main__':
    main()
