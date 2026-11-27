#!/usr/bin/env python3
"""Write patches/res-<W>x<H>.tsv: Bayonetta 3 1.2.0 rendering at a fixed internal resolution of W x H.

Where the game sets its sizes (read from main 1.2.0; the community "Force Scaling 900p" patch touches the same sites):
- main+0x69c6d0..e0: the display size by operation mode (docked 1600x896, handheld 1280x720), both set to W x H16;
- main+0x69c720 / 728: the NVN window (1600x900) -> W x H;
- main+0x69c73c / 740: the largest render size (1600x896) -> W x H16;
- main+0x69f0b8..25c: the render targets of the 7 dynamic-resolution levels (1600x896 down to 1024x576), all W x H16;
- main+0x182d670 / 570: the dynamic-resolution table (7 widths, 7 heights), all W x H16, so the level never drops.
H16 is H rounded down to a multiple of 16, as the game does (900 -> 896). Other buffers (2048x2048, the 1120x628 set)
keep their sizes. Every site is checked against the original instruction or data before the line is written; the
runtime checks it again. Larger sizes need more GPU memory than the game planned for: test them before relying on them.

usage: make_res_patch.py <W> <H> [--work BY3_build]
"""
import argparse
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(HERE / 'third_party/d2r'))   # Bayonetta 3's own copy of the rewriter
import nsoimg

# (offset, register, original immediate, 'W' | 'H' | 'H16')
MOVZ_SITES = [
    (0x69c6d0, 8, 0x500, 'W'), (0x69c6d4, 0, 0x640, 'W'), (0x69c6dc, 8, 0x2d0, 'H16'), (0x69c6e0, 9, 0x380, 'H16'),
    (0x69c720, 4, 0x640, 'W'), (0x69c728, 5, 0x384, 'H'),
    (0x69c73c, 0, 0x640, 'W'), (0x69c740, 1, 0x380, 'H16'),
    (0x69f0b8, 25, 0x640, 'W'), (0x69f0c8, 3, 0x380, 'H16'), (0x69f0f4, 3, 0x380, 'H16'), (0x69f114, 3, 0x380, 'H16'),
    (0x69f128, 3, 0x380, 'H16'),
    (0x69f140, 25, 0x5e0, 'W'), (0x69f144, 26, 0x350, 'H16'),
    (0x69f180, 25, 0x5a0, 'W'), (0x69f184, 26, 0x330, 'H16'),
    (0x69f1c0, 25, 0x550, 'W'), (0x69f1d0, 3, 0x300, 'H16'), (0x69f1e4, 3, 0x300, 'H16'),
    (0x69f1fc, 27, 0x500, 'W'), (0x69f200, 26, 0x2d0, 'H16'),
    (0x69f23c, 25, 0x240, 'H16'), (0x69f248, 2, 0x400, 'W'), (0x69f25c, 2, 0x400, 'W'),
]
TABLES = [(0x182d670, [0x640, 0x640, 0x5e0, 0x5a0, 0x550, 0x500, 0x400], 'W'),
          (0x182d570, [0x380, 0x380, 0x350, 0x330, 0x300, 0x2d0, 0x240], 'H16')]


def movz(reg, imm):
    return 0x52800000 | (imm << 5) | reg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('width', type=int); ap.add_argument('height', type=int)
    ap.add_argument('--work', type=Path, default=HERE / 'BY3_build')
    a = ap.parse_args()
    if not (640 <= a.width <= 3840 and 360 <= a.height <= 2160 and a.width % 32 == 0):
        sys.exit('width 640..3840 (a multiple of 32), height 360..2160')
    val = {'W': a.width, 'H': a.height, 'H16': a.height // 16 * 16}
    work = a.work.resolve()
    image = nsoimg.load(work / 'extracted/v120/exefs/main')
    rewritten = (work / 'rewritten/v120/main.patched.bin').read_bytes()
    rows = []
    for off, reg, orig, kind in MOVZ_SITES:
        old = struct.unpack_from('<I', image, off)[0]
        if old != movz(reg, orig): sys.exit(f'main+{off:#x}: expected movz w{reg}, #{orig:#x}, found {old:#010x}')
        rows.append((off, struct.pack('<I', movz(reg, val[kind])), image[off:off + 4]))
    for off, values, kind in TABLES:
        old = image[off:off + 4 * len(values)]
        if list(struct.unpack(f'<{len(values)}I', old)) != values: sys.exit(f'main+{off:#x}: unexpected table')
        rows.append((off, struct.pack(f'<{len(values)}I', *[val[kind]] * len(values)), old))
    for off, _, old in rows:
        if rewritten[off:off + len(old)] != old: sys.exit(f'main+{off:#x} lies on bytes the Apple-silicon rewrite changed')
    out = work / 'patches'; out.mkdir(exist_ok=True)
    name = f'res-{a.width}x{a.height}'
    (out / f'{name}.tsv').write_text(f'# internal resolution {a.width}x{a.height} (render {a.width}x{val["H16"]}; scripts/make_res_patch.py)\n'
                                     + ''.join(f'{off:#x}\t{new.hex()}\t{old.hex()}\n' for off, new, old in rows))
    print(f'{name}: {len(rows)} sites')


if __name__ == '__main__':
    main()
