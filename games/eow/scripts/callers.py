#!/usr/bin/env python3
"""Direct calls/branches (bl/b) to an address in an original module. usage: callers.py <module> 0x<offset> [...]"""
import struct, sys
from pathlib import Path
HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))
import nsoimg
mod, targets = sys.argv[1], {int(t, 16) for t in sys.argv[2:]}
img = nsoimg.load(str(HERE / f'EoW_build/extracted/v110/exefs/{mod}'))
text_end = next(int(l.split()[-1], 0) for l in (HERE / f'EoW_build/rewritten/v110/{mod}.fixups.tsv').read_text().splitlines() if 'TEXT_END' in l)
for a in range(0, text_end, 4):
    w = struct.unpack_from('<I', img, a)[0]
    if w >> 26 in (0b100101, 0b000101):
        imm = w & 0x3ffffff; imm = (imm ^ (1 << 25)) - (1 << 25)
        if a + imm * 4 in targets: print(f'{mod}+{a:#x} {"bl" if w >> 31 else "b"} {a + imm * 4:#x}')
