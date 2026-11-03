#!/usr/bin/env python3
"""Fuzz dm/tools/nacp.py, the reader of an NSP's control.nacp (the app name, display version and languages).
Seeds: a 0x4000-byte NACP with a name and publisher in two languages, a display version and a language flag, and one whose title area is compressed (u16 size + raw deflate).
Clean rejection: none raises; name() and version() return '' and languages() returns [] for data they cannot read (their docstrings). Any exception is a finding, and so is a
result of another type. Usage: see fuzzlib.py"""
import struct
import sys
import zlib

import fuzzlib
import nacp

HARNESS = 'nacp'
fuzzlib.guard(nacp)


def title_area(names):   # 0x300 bytes per language: name 0x200 + publisher 0x100
    t = bytearray(0x300 * 16)
    for i, (name, pub) in names.items(): t[i * 0x300:i * 0x300 + len(name)] = name; t[i * 0x300 + 0x200:i * 0x300 + 0x200 + len(pub)] = pub
    return t


def seeds():
    plain = bytearray(0x4000); plain[:0x3000] = title_area({0: (b'Fuzz Title', b'Fuzz Publisher'), 2: (b'Fuzz Title JP', b'Pub')})
    struct.pack_into('<I', plain, 0x302C, 0b101); plain[0x3060:0x3066] = b'1.2.3\0'
    packed = bytearray(plain)
    z = zlib.compressobj(9, zlib.DEFLATED, -15); area = z.compress(bytes(title_area({1: (b'Fuzz Compressed', b'Pub')})) + bytes(0x300 * 16)) + z.flush()
    packed[:0x3000] = bytes(0x3000); struct.pack_into('<H', packed, 0, len(area)); packed[2:2 + len(area)] = area
    return {'plain': bytes(plain), 'compressed': bytes(packed)}


_file = fuzzlib.InputFile('control.nacp')


def check(data):
    p = _file.write(data)
    n, v, langs = nacp.name(p), nacp.version(p), nacp.languages(p)
    if not isinstance(n, str) or not isinstance(v, str) or not isinstance(langs, list): raise fuzzlib.broken('nacp.py', f'unexpected result types {n!r} {v!r} {langs!r}')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
