#!/usr/bin/env python3
"""Summarize relocations in verified base-game modules without exporting their code or symbol names."""
import argparse
import collections
import json
import struct
import sys
from pathlib import Path

from rewrite_modules import GAME, MODULES, verify_exefs

sys.path.insert(0, str(GAME / 'third_party/d2r'))
from nsoimg import load, relocs


def summarize(records):
    kinds = collections.Counter(str(record[1]) for record in records)
    symbols = {record[2] for record in records if record[2]}
    imports = {record[2] for record in records if record[2] and record[3] is None}
    return {'relocations': len(records), 'relocation_kinds': dict(sorted(kinds.items())),
            'referenced_symbols': len(symbols), 'undefined_symbols': len(imports)}


def data_bounds(path):
    with path.open('rb') as src:
        header = src.read(0x100)
    if len(header) != 0x100 or header[:4] != b'NSO0':
        raise ValueError('invalid NSO header')
    data_offset, data_size, bss_size = struct.unpack_from('<III', header, 0x34)
    return data_offset, (data_offset + data_size + bss_size + 0x3fff) & ~0x3fff


def validate_relocations(records, data_start, data_end):
    """Reject unsupported, overlapping, or out-of-range relocation targets."""
    seen = []
    for offset, kind, name, value, addend in records:
        if kind not in (257, 1025, 1026, 1027):
            raise ValueError(f'unsupported relocation kind: {kind}')
        if offset < data_start or offset + 8 > data_end:
            raise ValueError('relocation target outside module data')
        if kind == 1027 and not 0 <= addend < data_end:
            raise ValueError('relative relocation addend outside module image')
        if kind == 1026 and addend != 0:
            raise ValueError('unsupported jump-slot addend')
        seen.append(offset)
    seen.sort()
    if any(b < a + 8 for a, b in zip(seen, seen[1:])):
        raise ValueError('overlapping relocation targets')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', required=True, type=Path)
    args = parser.parse_args()
    exefs = args.exefs.resolve()
    verify_exefs(exefs)
    results = {}
    for name in MODULES:
        records = relocs(load(exefs / name, bss=False))
        validate_relocations(records, *data_bounds(exefs / name))
        results[name] = summarize(records)
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
