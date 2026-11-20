#!/usr/bin/env python3
"""Split a combined game card image (a merged XCI, "1G+1U+3D": the base game, its update and add-ons on one card) into
one NSP per title, which the port's tools read as they read separate packages.

    python3 -m games.acnh.tools.split_card <card.xci> --keys <prod.keys> --output <folder>

Each title's contents are the NCAs its verified content meta (CNMT) lists, so a base game and an update whose Program
NCAs share the base title ID are told apart by the meta, not by the NCA headers. The NCAs are copied byte for byte (no
decryption): a card's NCAs carry their keys in the key area, and a rights-managed one is refused, since a card keeps no
tickets. <folder>/split.json records the card it came from, so a later build of the same card reuses the NSPs."""
import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
    __package__ = 'games.acnh.tools'

from .extract_exefs import common_tickets, container_entries, load_keys, nca_inventory, require  # noqa: E402
from .identity import verified_cnmt  # noqa: E402

META_TYPES = {'base': 0x80, 'update': 0x81, 'dlc': 0x82}
COPY_CHUNK = 16 << 20
DELTA_FRAGMENT = 6   # a content meta's content type


def role_of(title_id, base_id):
    title = int(title_id, 16)
    if title == base_id:
        return 'base'
    if title == base_id + 0x800:
        return 'update'
    require(title >> 12 == (base_id + 0x1000) >> 12, f'{title_id} is not a title of this card\'s game')
    return 'dlc'


def card_titles(card, keys, base_id):
    """[(role, title ID, version, [(name, offset, size)])] of each title on the card, from its content metas."""
    with card.open('rb') as f:
        kind, entries = container_entries(f, card.stat().st_size)
        require(kind == 'xci', 'Expected a game card image (XCI)')
        tickets = common_tickets(f, entries)
        records = nca_inventory(f, entries, keys)
        require(all(h[0x230:0x240] == bytes(16) for _, h in records),
                'A rights-managed NCA on a card without tickets: this card cannot be split')
        by_name = {name: (name, offset, size) for name, offset, size in entries}
        titles = []
        for record, header in records:
            if record['content_type'] != 1:
                continue
            role = role_of(record['title_id'], base_id)
            meta = verified_cnmt(f, record, header, keys, tickets, META_TYPES[role])
            require(meta is not None, f'The content meta of {record["title_id"]} cannot be read')
            files = [by_name[record['name']]]
            for content, content_type in meta['contents']:
                name = f'{content}.nca'
                if content_type == DELTA_FRAGMENT and name not in by_name:
                    continue   # an update's delta from an earlier version, which a merged card leaves out
                require(name in by_name, f'{name} of {record["title_id"]} is not on the card')
                files.append(by_name[name])
            titles.append((role, record['title_id'], meta['cnmt_version_raw'], files))
    return titles


def pfs0_header(files):
    names = b''.join(name.encode() + b'\0' for name, _, _ in files)
    table = 16 + len(files) * 24
    names += b'\0' * (-(table + len(names)) % 0x20)
    out, offset, name_offset = bytearray(b'PFS0' + struct.pack('<II', len(files), len(names)) + bytes(4)), 0, 0
    for name, _, size in files:
        out += struct.pack('<QQII', offset, size, name_offset, 0)
        offset += size
        name_offset += len(name.encode()) + 1
    return bytes(out) + names


def write_nsp(card, files, path):
    part = path.with_suffix('.part')
    with card.open('rb') as src, part.open('wb') as dst:
        dst.write(pfs0_header(files))
        for _, offset, size in files:
            src.seek(offset)
            left = size
            while left:
                chunk = src.read(min(COPY_CHUNK, left))
                require(chunk, 'The card image ends early')
                dst.write(chunk)
                left -= len(chunk)
    part.replace(path)


def stamp(path):
    st = path.stat()
    return [str(path.resolve()), st.st_size, st.st_mtime_ns]


def split(card, keys_file, output, base_id):
    """{'base': Path, 'update': Path | None, 'update_version': int | None, 'dlc': [Path]} of the card's titles, as NSPs in
    output (reused when split.json says they were made from this card)."""
    card, output = Path(card), Path(output)
    record = output / 'split.json'
    try:
        done = json.loads(record.read_text())
        if done['card'] == stamp(card) and all((output / n).is_file() for n in done['files']):
            return result(output, done)
    except (OSError, ValueError, KeyError):
        pass
    titles = card_titles(card, load_keys(Path(keys_file)), base_id)
    require(sum(role == 'base' for role, *_ in titles) == 1, 'Expected one base game on the card')
    require(sum(role == 'update' for role, *_ in titles) <= 1, 'Expected at most one update on the card')
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    done = {'card': None, 'files': [], 'titles': []}
    for role, title, version, files in titles:
        name = f'{role}-{title}.nsp'
        write_nsp(card, files, output / name)
        done['files'].append(name)
        done['titles'].append({'role': role, 'title_id': title, 'version': version, 'file': name})
    done['card'] = stamp(card)
    record.write_text(json.dumps(done, indent=2) + '\n')
    return result(output, done)


def result(output, done):
    by_role = lambda role: [t for t in done['titles'] if t['role'] == role]
    update = by_role('update')
    return {'base': output / by_role('base')[0]['file'],
            'update': output / update[0]['file'] if update else None,
            'update_version': update[0]['version'] if update else None,
            'dlc': [output / t['file'] for t in by_role('dlc')]}


def is_combined(card):
    """True when a game card image holds more than one title (more than one content meta), read without keys."""
    try:
        with Path(card).open('rb') as f:
            kind, entries = container_entries(f, Path(card).stat().st_size)
    except (OSError, ValueError):
        return False
    return kind == 'xci' and sum(name.endswith('.cnmt.nca') for name, _, _ in entries) > 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('card', type=Path)
    parser.add_argument('--keys', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--title-id', default='01006F8002326000', help='the base game\'s title ID')
    args = parser.parse_args()
    found = split(args.card, args.keys, args.output, int(args.title_id, 16))
    print(json.dumps({k: (str(v) if isinstance(v, Path) else [str(d) for d in v] if isinstance(v, list) else v)
                      for k, v in found.items()}, indent=2))


if __name__ == '__main__':
    main()
