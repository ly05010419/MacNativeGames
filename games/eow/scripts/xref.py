#!/usr/bin/env python3
"""Find code in an original module that forms an address (adrp + add/ldr on the same register).
usage: xref.py <module> 0x<offset> [0x<offset> ...]"""
import struct, sys
from pathlib import Path
HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))
import nsoimg

mod, targets = sys.argv[1], {int(t, 16) for t in sys.argv[2:]}
img = nsoimg.load(str(HERE / f'EoW_build/extracted/v110/exefs/{mod}'))
text_end = next(int(l.split()[-1], 0) for l in (HERE / f'EoW_build/rewritten/v110/{mod}.fixups.tsv').read_text().splitlines() if 'TEXT_END' in l)
pages = {t & ~4095 for t in targets}
for a in range(0, text_end, 4):
    w = struct.unpack_from('<I', img, a)[0]
    if w & 0x9f000000 != 0x90000000: continue
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2); imm = (imm ^ (1 << 20)) - (1 << 20)
    page = (a & ~4095) + (imm << 12)
    if page not in pages: continue
    rd = w & 31
    for b in range(a + 4, min(a + 64, text_end), 4):
        v = struct.unpack_from('<I', img, b)[0]
        if (v & 0xffc00000) == 0x91000000 and ((v >> 5) & 31) == rd: off = (v >> 10) & 0xfff
        elif (v & 0xffc00000) == 0xf9400000 and ((v >> 5) & 31) == rd: off = ((v >> 10) & 0xfff) * 8
        else: continue
        if page + off in targets: print(f'{mod}+{a:#x} -> {page + off:#x}')
        break
