#!/usr/bin/env python3
"""Fuzz dm/tools/nso.py's load, the one loader of NSO and NRO modules that every tool reads a game's modules through (shader_split.nso_image, uepak.nso_image, patch_nso.py and
the others call it).
Seeds: an NSO0 with three small segments, text LZ4-compressed, ro and data stored (and the same with every segment compressed), and an NRO0 of three one-page segments.
Clean rejection: nso.Error (a ValueError), which load raises for every module it does not read. The loaded image must have the length its segments lay out (the data segment's
offset + size): a shorter or longer one means segments were laid out at the wrong place. Any other exception, a wrong length, or an allocation beyond fuzzlib.allowed() (a
segment size or offset far past what the file can hold) is a finding. Usage: see fuzzlib.py"""
import struct
import sys

import fuzzlib
import lz4.block
import nso as nso_mod

HARNESS = 'nso'
fuzzlib.guard(nso_mod)


def nso(segments, compress):
    """NSO0: +0xc flags (bit i: segment i compressed), +0x10 + 0x10 i {file offset, memory offset, size}, +0x3c bss, +0x40 module ID, +0x60 stored sizes"""
    head, body, mem, stored = bytearray(0x100), bytearray(), 0, []
    head[:4] = b'NSO0'
    for i, seg in enumerate(segments):
        raw = lz4.block.compress(seg, store_size=False) if compress[i] else seg
        struct.pack_into('<III', head, 0x10 + 0x10 * i, 0x100 + len(body), mem, len(seg))
        body += raw; stored.append(len(raw)); mem += (len(seg) + 0xfff) & ~0xfff
    struct.pack_into('<I', head, 0xc, sum(1 << i for i, c in enumerate(compress) if c))
    struct.pack_into('<I', head, 0x3c, 0x1000); head[0x40:0x60] = bytes(range(32)); struct.pack_into('<III', head, 0x60, *stored)
    return bytes(head + body)


def nro(segments):
    """NRO0 (the image itself): +0x10 magic, +0x18 module size, +0x20 + 8 i {offset, size} of text, ro, data (whole pages, end to end), +0x38 bss, +0x40 build ID"""
    d = bytearray(b''.join(s.ljust((len(s) + 0xfff) & ~0xfff, b'\0') for s in segments))
    d[0x10:0x14] = b'NRO0'; struct.pack_into('<I', d, 0x18, len(d)); at = 0
    for i, s in enumerate(segments):
        n = (len(s) + 0xfff) & ~0xfff; struct.pack_into('<II', d, 0x20 + 8 * i, at, n); at += n
    struct.pack_into('<I', d, 0x38, 0x1000); d[0x40:0x60] = bytes(range(32))
    return bytes(d)


def seeds():
    text = bytes.fromhex('fd7bbfa9fd030091') * 32 + bytes.fromhex('c0035fd6')   # stp / mov / ret
    ro, data = b'fuzz ro segment\0' * 8, struct.pack('<4Q', 1, 2, 3, 4)
    return {'text-lz4': nso([text, ro, data], (1, 0, 0)), 'all-lz4': nso([text, ro, data], (1, 1, 1)), 'nro': nro([bytes(0x80) + text, ro, data])}


def check(data):
    try: m = nso_mod.load(data, 'main')
    except nso_mod.Error: return   # the declared rejection
    mo, sz = m.segments[2]
    if len(m.image) != mo + sz: raise fuzzlib.broken('nso.py:load', f'the image is {len(m.image):#x} bytes, the segments lay out {mo + sz:#x}')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
