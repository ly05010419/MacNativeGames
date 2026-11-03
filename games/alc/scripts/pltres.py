#!/usr/bin/env python3
"""Resolve main's PLT targets and called addresses to import names.

Usage: pltres.py <addr> [addr...]   (addresses are main-relative, 0x prefix ok)
Builds the map from the rewritten main's PLT region: each stub is adrp x17, page; ldr x17, [x17, #lo];
the GOT slot holds a JUMP_SLOT relocation whose symbol names the import. Data addresses resolve against
the module table's symbols too.
"""
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'third_party/d2r'))
import nsoimg
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

WORK = Path(__file__).resolve().parents[1] / 'AC_build'
VERSION = 'v101'


def build():
    tables = (WORK / 'build/runtime_tables.h').read_text()
    relocs = re.search(r'static const Reloc reloc_0\[\] = \{(.*?)\};', tables, re.S).group(1)
    symbols = re.search(r'static const Symbol symbol_0\[\] = \{(.*?)\};', tables, re.S).group(1)
    sym_list = re.findall(r'\{"((?:[^"\\]|\\.)*)",(\d+)ULL,', symbols)
    got = {}
    for m in re.finditer(r'\{(\d+)ULL,1026,(\d+),(-?\d+)LL\}', relocs):
        where, si, addend = int(m.group(1)), int(m.group(2)), int(m.group(3))
        if si < len(sym_list):
            got[where] = sym_list[si][0].replace('\\u005f', '_')
    img = nsoimg.load(WORK / f'extracted/{VERSION}/exefs/main')
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    h = {}
    for line in (WORK / f'rewritten/{VERSION}/main.fixups.tsv').read_text().splitlines():
        if line.startswith('#'):
            k, v = line[1:].split(); h[k] = int(v, 0)
    text_end = h['TEXT_END']
    plt = {}
    pos = text_end - 0x40000
    while pos < text_end - 12:
        a, b, c, d = struct.unpack_from('<IIII', img, pos)
        if a & 0x9f00001f == 0x90000010 and b & 0xffc003ff == 0xf9400211 and d == 0xd61f0220:
            imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
            imm = (imm ^ (1 << 20)) - (1 << 20)
            slot = ((pos & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8)
            if slot in got:
                plt[pos] = got[slot]
        pos += 4
    return plt, got, sym_list


def main():
    plt, got, syms = build()
    for a in sys.argv[1:]:
        addr = int(a, 0)
        name = plt.get(addr)
        if name:
            print(f'main+{addr:x}  PLT -> {name}')
            continue
        # a bl into the sdk? try data symbols
        near = [(v, n) for v, n in syms if n and abs(int(v) - addr) < 0x20000 and int(v) <= addr]
        print(f'main+{addr:x}  (no PLT; local function)')


if __name__ == '__main__':
    main()
