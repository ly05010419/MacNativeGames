#!/usr/bin/env python3
"""Make a new diagnostic profile copy with a guarded saved spawn position.

Input must use a verified 1.0.0/1.1.2 save layout (1.2.1 writes the 1.1.2 one), which 1.4.3 can migrate when
loading. This deliberately does not assume a layout for newly written saves.
Only the new copy is edited; existing destinations are always refused.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'diag'))
from save_position import locate_all


def manifest(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in root.rglob('*') if p.is_file()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--xyz', type=float, nargs=3, required=True)
    args = parser.parse_args()
    source, dest = args.source.resolve(), args.destination.resolve()
    if dest.exists() or source == dest or source in dest.parents or dest in source.parents:
        parser.error('requires a new destination outside the source profile')
    x, y, z = args.xyz
    if not all(math.isfinite(v) for v in args.xyz) or not (-5500 <= x <= 5500 and -1500 <= y <= 3500 and -5000 <= z <= 5000):
        parser.error('coordinates outside the diagnostic world range')
    saves = list(source.glob('save/*/slot_*/progress.sav'))
    if not saves:
        parser.error('source profile has no saved progress')
    before = manifest(source)
    edits = []
    # Validate every input before creating the destination.
    for path in saves:
        raw = path.read_bytes()
        offsets = locate_all(raw)
        for off in offsets:
            if not all(math.isfinite(v) for v in struct.unpack_from('<fff', raw, off)):
                raise ValueError('non-finite source position')
        edits.append((path.relative_to(source), raw, offsets))
    shutil.copytree(source, dest)
    for relative, raw, offsets in edits:
        changed = bytearray(raw)
        for off in offsets:
            struct.pack_into('<fff', changed, off, *args.xyz)
        allowed = {i for off in offsets for i in range(off, off + 12)}
        assert all(a == b or i in allowed for i, (a, b) in enumerate(zip(raw, changed)))
        assert len(changed) == len(raw)
        (dest / relative).write_bytes(changed)
    assert manifest(source) == before, 'source profile changed during staging'
    report = dict(status='pass', edited_save_slots=len(edits), xyz=args.xyz,
                  source_files_unchanged=len(before), scope='new private save copy; only two guarded position vectors per slot')
    (dest.parent / (dest.name + '-staging.json')).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
