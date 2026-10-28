#!/usr/bin/env python3
"""Verify and merge the Mario Kart 8 Deluxe base AES-CTR RomFS with its BKTR update.

Game data and keys remain local. The update IVFC authenticates every output block.
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/by3-scripts'))
from extract_exefs import (load_keys, container_entries, common_tickets,
                           nca_inventory, nca_ctr_key, require)
from extract_romfs import index_romfs
from mk8d import TITLE

spec = importlib.util.spec_from_file_location('by3_merge_romfs', HERE / 'third_party/by3-scripts/merge_romfs.py')
by3_merge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(by3_merge)
Ctr, Patched = by3_merge.Ctr, by3_merge.Patched
romfs_section, verify_ivfc = by3_merge.romfs_section, by3_merge.verify_ivfc


class DirectBase:
    def __init__(self, ctr, start, end, fs):
        require(fs[4] == 3, 'Expected a direct AES-CTR base RomFS')
        require(end <= ctr.size, 'Base RomFS exceeds stored NCA')
        self.ctr, self.start, self.size = ctr, start, end - start
        self.upper = fs[0x140:0x148][::-1]

    def read(self, offset, size):
        require(0 <= offset and offset + size <= self.size, 'Base RomFS read out of bounds')
        return self.ctr.read(self.start + offset, size, self.upper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path, help='Local combined base + update NSP (scripts/merge_nsp.py) or XCI')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    require(not args.output.exists(), 'Output already exists')
    keys = load_keys(args.keys)
    with args.game.open('rb') as f:
        _, entries = container_entries(f, args.game.stat().st_size)
        records = nca_inventory(f, entries, keys)
        programs = [(r, h) for r, h in records if r['content_type'] == 0 and r['title_id'] == TITLE]
        require(len(programs) == 2, 'Expected exactly one base and one update Program NCA')
        sections = [(r, h, romfs_section(h)) for r, h in programs]
        base_items = [(r, h, s) for r, h, s in sections if s[2][4] == 3]
        update_items = [(r, h, s) for r, h, s in sections if s[2][4] == 4]
        require(len(base_items) == len(update_items) == 1, 'Expected direct base and BKTR update RomFS')
        brec, bhdr, (bstart, bend, bfs) = base_items[0]
        urec, uhdr, (ustart, uend, ufs) = update_items[0]
        tickets = common_tickets(f, entries)
        base_ctr = Ctr(f, brec['nca_offset'], brec['size'], nca_ctr_key(bhdr, keys, tickets))
        update_ctr = Ctr(f, urec['nca_offset'], urec['size'], nca_ctr_key(uhdr, keys, tickets))
        base = DirectBase(base_ctr, bstart, bend, bfs)
        patched = Patched(update_ctr, ustart, uend, ufs, base)
        args.output.mkdir(parents=True)
        image = args.output / 'romfs.bin'
        counts, size, digest = verify_ivfc(ufs, patched.total, patched.read, image)
        files = index_romfs(image)
        report = dict(status='pass', base_program=brec, update_program=urec,
                      source_size=args.game.stat().st_size, ivfc_blocks=counts,
                      romfs_size=size, romfs_sha256=digest,
                      bytes_from_base=patched.from_base, bytes_from_update=patched.from_update,
                      files=files,
                      integrity='FS headers and all six update IVFC levels verified; RSA signatures not checked')
        (args.output / 'index.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(status='pass', files=len(files), size=size, sha256=digest,
                              from_base=patched.from_base, from_update=patched.from_update)), flush=True)


if __name__ == '__main__':
    main()
