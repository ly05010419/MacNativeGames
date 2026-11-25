#!/usr/bin/env python3
"""Extract and verify Age of Calamity's add-on content (DLC) from the player's own NSPs into <work>/extracted/aoc/<index>.

    python3 scripts/extract_dlc.py <DLC NSPs or a folder of them> --keys prod.keys --work <work> [--measure]

An add-on NSP holds one data NCA whose title is the game's add-on title 01002B00111A3000 + index (1: the bonus item,
2-4: the Expansion Pass waves). Its RomFS is decrypted with the NSP's common ticket, all six IVFC levels are verified
while the image streams to disk, and its files are written to extracted/aoc/<index>, the folder runtime/fs_host.c lists
(nn::aoc::ListAddOnContent) and mounts read-only (nn::fs::MountAddOnContent), as Mario Kart 8 Deluxe's Booster Course Pass;
the report goes beside it (<index>.json), never into the mounted folder. The data NCA (name, size) and the RomFS (size,
SHA-256, file count) must match assets/expected-dlc.json; anything else stops. Two files of the same add-on (a copy) are
read once. --measure prints those values and keeps nothing. Keys stay in memory. No executable launch, no network.
games/pla's NCA readers and IVFC check are used as they are; the RomFS section reader is scripts/nca_romfs_section.py.
"""
import argparse
import json
import shutil
import sys
from pathlib import Path

GAMES = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(GAMES / 'pla/scripts'))
from extract_exefs import require, load_keys, container_entries, common_tickets, nca_ctr_key, nca_inventory  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402
from merge_romfs import CHUNK, Ctr, verify_ivfc  # noqa: E402
from nca_romfs_section import romfs_section  # noqa: E402

HERE = Path(__file__).resolve().parents[1]
MANIFEST = HERE / 'assets/expected-dlc.json'
AOC_BASE = 0x01002B00111A2000 + 0x1000
DATA, PUBLIC_DATA = 4, 5


def addon_files(paths):
    """The NSPs named or inside the folders named, in name order."""
    out = []
    for p in paths:
        out.extend(sorted(q for q in p.iterdir() if q.suffix.lower() == '.nsp') if p.is_dir() else [p])
    return out


def open_addon(path, keys):
    """The NSP's one add-on data NCA: (file, record, header, Ctr, index)."""
    f = path.open('rb')
    try:
        _, entries = container_entries(f, path.stat().st_size)
        data = [(r, h) for r, h in nca_inventory(f, entries, keys) if r['content_type'] in (DATA, PUBLIC_DATA)]
        require(len(data) == 1, f'{path.name}: expected one add-on data NCA, found {len(data)}')
        record, hdr = data[0]
        index = int(record['title_id'], 16) - AOC_BASE
        require(1 <= index < 0x1000, f'{path.name}: title {record["title_id"]} is not an Age of Calamity add-on')
        key = nca_ctr_key(hdr, keys, common_tickets(f, entries))
    except BaseException:
        f.close()
        raise
    return f, record, hdr, Ctr(f, record['nca_offset'], record['size'], key), index


def extract_files(image, files, root):
    """Every indexed file of the verified image under root (paths checked by index_romfs: no '..', no absolute)."""
    with image.open('rb') as src:
        for entry in files:
            out = root / entry['path']
            out.parent.mkdir(parents=True, exist_ok=True)
            with out.open('wb') as dst:
                src.seek(entry['offset'])
                left = entry['size']
                while left:
                    chunk = src.read(min(CHUNK, left))
                    require(chunk, f'RomFS image ends inside {entry["path"]}')
                    dst.write(chunk)
                    left -= len(chunk)


def extract(path, keys, work, pinned_all, measure, done):
    f, record, hdr, ctr, index = open_addon(path, keys)
    with f:
        if index in done:
            require(done[index] == (record['name'], record['size']), f'{path.name}: add-on {index} again, with another data NCA')
            print(f'{path.name}: add-on {index} already read from another file (the same data NCA)', flush=True)
            return
        done[index] = (record['name'], record['size'])
        pinned = pinned_all.get(str(index))
        if not measure:
            require(pinned is not None, f'{path.name}: add-on {index} is not in {MANIFEST.name}')
            require((record['name'], record['size']) == (pinned['data_nca']['name'], pinned['data_nca']['size']),
                    f'{path.name}: its data NCA is not the pinned one for add-on {index}')
        aoc = work / 'extracted/aoc'
        target, report_path = aoc / str(index), aoc / f'{index}.json'
        if not measure and target.is_dir() and report_path.is_file():
            print(f'{path.name}: add-on {index} already extracted', flush=True)
            return
        require(measure or not target.exists(), f'{target} exists without its report; remove it to extract again')
        staging = aoc / f'.incoming-{index}'
        require(not staging.exists(), f'{staging} is left from an interrupted run; remove it first')
        staging.mkdir(parents=True)
        try:
            start, end, fs = romfs_section(hdr)
            upper = fs[0x140:0x148][::-1]
            image = staging / 'romfs.bin'
            counts, size, sha = verify_ivfc(fs, end - start, lambda offset, n: ctr.read(start + offset, n, upper), image)
            files = index_romfs(image)
            measured = dict(title=record['title_id'], data_nca=dict(name=record['name'], size=record['size']),
                            romfs=dict(size=size, sha256=sha, files=len(files)))
            if measure:
                print(json.dumps({str(index): measured}, indent=4), flush=True)
                return
            require(measured['romfs'] == pinned['romfs'], f'{path.name}: its RomFS does not match the pinned add-on {index}')
            extract_files(image, files, staging / 'files')
            image.unlink()
            report = dict(status='pass', index=index, source=path.name, source_size=path.stat().st_size, ivfc_blocks=counts,
                          **measured, file_list=[dict(path=e['path'], size=e['size']) for e in files],
                          integrity='FS header and all six IVFC levels of the add-on RomFS verified; RSA signatures not checked')
            (staging / 'files').rename(target)
            report_path.write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(dict(status='pass', index=index, title=record['title_id'], files=len(files), size=size, sha256=sha)), flush=True)
        finally:
            shutil.rmtree(staging, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('addons', type=Path, nargs='+', help='add-on NSPs, or folders of them')
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    parser.add_argument('--measure', action='store_true', help='print the values to pin for these add-ons; keep nothing')
    args = parser.parse_args()
    pinned = json.loads(MANIFEST.read_text())['addons'] if MANIFEST.exists() else {}
    keys, done = load_keys(args.keys), {}
    for path in addon_files(args.addons):
        extract(path, keys, args.work.resolve(), pinned, args.measure, done)


if __name__ == '__main__':
    main()
