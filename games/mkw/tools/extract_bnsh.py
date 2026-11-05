#!/usr/bin/env python3
"""Extract NVN2 stages from MKW's .bfsha.zs and .sarc.zs shader archives into a private capture folder.

Requires zstandard. Output contains game code and must not be committed or shared.
BNSH descriptor layout follows nxnative shader_split.py (MIT), commit 65501fbb.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path
import zstandard

MAGIC = bytes.fromhex('34127698c9000000')


def stages(data):
    def unpack(fmt, offset):
        n = struct.calcsize(fmt)
        if offset < 0 or offset + n > len(data):
            raise ValueError(f'BNSH pointer outside archive: {offset:#x}')
        return struct.unpack_from(fmt, data, offset)
    at = 0
    while True:
        base = data.find(b'BNSH\0\0\0\0', at)
        if base < 0:
            return
        at = base + 8
        variants, table = unpack('<IQ', base + 0x7c)
        if variants > len(data) // 0x40:
            raise ValueError('implausible BNSH variation count')
        for variant in range(variants):
            program, = unpack('<Q', base + table + 0x40 * variant + 0x10)
            if not program:
                continue
            for slot in range(6):
                desc, = unpack('<Q', base + program + 8 + slot * 8)
                if not desc:
                    continue
                ctl, code, size, ctl_size = unpack('<QQII', base + desc + 8)
                c, b = base + ctl, base + code
                if c + ctl_size > len(data) or b + size > len(data) or ctl_size < 0xbc:
                    raise ValueError('truncated stage descriptor')
                control, binary = data[c:c + ctl_size], data[b:b + size]
                if control[:8] != MAGIC:
                    raise ValueError('stage is not NVN2 v0xc9')
                inst, = unpack('<I', c + 0x38)
                c1, end = unpack('<II', c + 0x40)
                if not 0 < inst <= c1 <= end == size:
                    raise ValueError('inconsistent NVN2 code lengths')
                yield base, variant, slot, control, binary


def bahs_stages(data):
    if len(data) < 0x20 or data[:4] != b'BAHS':
        raise ValueError('invalid BAHS header')
    size, region = struct.unpack_from('<II', data, 0x18)
    if region < 0x20 or region + size > len(data):
        raise ValueError('BAHS code region outside file')
    for entry in range(0x20, region - 15, 4):
        offset, zero, ctl_size, relative = struct.unpack_from('<IIIi', data, entry)
        ctl = entry + relative
        if zero or offset % 256 or not 0xbc <= ctl_size < 0x10000 or ctl < 0 or ctl + ctl_size > region:
            continue
        control = data[ctl:ctl + ctl_size]
        if control[:8] != MAGIC:
            continue
        inst = struct.unpack_from('<I', control, 0x38)[0]
        c1, end = struct.unpack_from('<II', control, 0x40)
        if not 0 < inst <= c1 <= end or offset + end > size:
            raise ValueError('inconsistent BAHS code lengths')
        yield 0, entry, control[0x56], control, data[region + offset:region + offset + end]


def sarc_members(data):
    if len(data) < 0x20 or data[:4] != b'SARC':
        raise ValueError('invalid SARC header')
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    sfat = struct.unpack_from(endian + 'H', data, 4)[0]
    region = struct.unpack_from(endian + 'I', data, 12)[0]
    if sfat + 12 > len(data) or data[sfat:sfat + 4] != b'SFAT':
        raise ValueError('invalid SARC file table')
    count = struct.unpack_from(endian + 'H', data, sfat + 6)[0]
    names = sfat + 12 + 16 * count + 8
    if names > region or region > len(data):
        raise ValueError('SARC table outside file')
    for i in range(count):
        _, name, start, end = struct.unpack_from(endian + 'IIII', data, sfat + 12 + i * 16)
        pos = names + (name & 0xffffff) * 4
        terminator = data.find(b'\0', pos, region)
        if pos >= region or terminator < 0 or start > end or region + end > len(data):
            raise ValueError('invalid SARC member')
        yield data[pos:terminator].decode(), data[region + start:region + end]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('shader_directory', type=Path)
    p.add_argument('output', type=Path)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    rows, unique = [], set()
    archives = sorted(args.shader_directory.glob('*.bfsha.zs')) + sorted(args.shader_directory.glob('*.sarc.zs'))
    for archive in archives:
        data = zstandard.ZstdDecompressor().decompress(archive.read_bytes(), max_output_size=512 << 20)
        members = sarc_members(data) if data[:4] == b'SARC' else [(archive.name, data)]
        for member, body in members:
            records = bahs_stages(body) if body[:4] == b'BAHS' else stages(body)
            for base, variant, slot, ctl, code in records:
                write_stage(args.output, unique, rows, archive.name, member, base, variant, slot, ctl, code)
    if not rows:
        p.error('no shader stages found')
    (args.output / 'archive-index.json').write_text(json.dumps(rows, indent=2) + '\n')
    print(f'{len(rows)} stage references; {len(unique)} unique shaders')


def write_stage(output, unique, rows, archive, member, base, variant, slot, ctl, code):
    canonical = bytearray(ctl[:0x48]); canonical[0x30:0x34] = bytes(4)
    key = hashlib.sha256(canonical + code).hexdigest()
    if key not in unique:
        (output / (key + '.ctl')).write_bytes(ctl)
        (output / (key + '.bin')).write_bytes(code)
        unique.add(key)
    rows.append({'archive': archive, 'member': member, 'container_offset': base, 'variant': variant, 'slot': slot, 'nvn_stage': ctl[0x56], 'key': key})


if __name__ == '__main__':
    main()
