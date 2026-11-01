#!/usr/bin/env python3
"""Generate the internal-1920x1080 exefs patch for Astral Chain 1.0.1 (mods/1.0.1/res-1920x1080/).

The game picks its docked render size with `mov w9, #0x640` (1600) / `mov w9, #0x384` (900) beside a csel against the
handheld pair (1280x720); the display-size getter at main+0x4328c0 returns the same pair, and three compares test a
width of 1600 to decide "docked". Raising all of them to 1920x1080 keeps the game's own size arithmetic consistent (its
dynamic-resolution levels scale from this base); the handheld pair is left alone. Sites were read from main 1.0.1 with
capstone; this script is the by3 make_res_patch.py's role for this game (that copy targeted BY3_build/v120 and could
not run here — the numbers below are Astral Chain's own).

Every site is verified against the decompressed main image before it is written, and every replacement word is decoded
back to the intended instruction here (the runtime checks the bytes again at load).

Usage: AC_build/venv/bin/python scripts/make_res_patch.py   (from games/alc)
"""
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

HERE = Path(__file__).resolve().parents[1]
IMAGE = HERE / 'AC_build/extracted/v101/main.decompressed.bin'
OUT = HERE / 'mods/1.0.1/res-1920x1080/exefs/res-1920x1080.pchtxt'
BUILD_ID = '4B159F0F7A360669738390EE8657B4F000000000000000000000000000000000'
W, H = 1920, 1080
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
img = IMAGE.read_bytes()


def word(off):
    return struct.unpack_from('<I', img, off)[0]


def dec(w, at):
    return next(md.disasm(struct.pack('<I', w), at))


def mov_w(reg, imm):
    w = 0x52800000 | ((imm & 0xFFFF) << 5) | reg   # movz wN, #imm (fits 16 bits)
    d = dec(w, 0)
    assert d.mnemonic == 'mov' and d.op_str == f'w{reg}, #{imm:#x}', d.mnemonic + ' ' + d.op_str
    return w


def cmp_imm(rn, imm):
    w = 0x7100001F | ((imm & 0xFFF) << 10) | (rn << 5)   # SUBS wzr, wN, #imm
    d = dec(w, 0)
    assert d.mnemonic == 'cmp' and d.op_str == f'w{rn}, #{imm:#x}', d.mnemonic + ' ' + d.op_str
    return w


# (offset, expected instruction, replacement, what)
SITES = [
    (0x40D5E4, 'mov w9, #0x640', mov_w(9, W), 'docked width (size chooser 1)'),
    (0x40D5F0, 'mov w9, #0x384', mov_w(9, H), 'docked height (size chooser 1)'),
    (0x40FB04, 'mov w9, #0x640', mov_w(9, W), 'docked width (size chooser 2)'),
    (0x40FB14, 'mov w9, #0x384', mov_w(9, H), 'docked height (size chooser 2)'),
    (0x4328C0, 'mov w8, #0x640', mov_w(8, W), 'display-size getter: width'),
    (0x4328C8, 'mov w8, #0x384', mov_w(8, H), 'display-size getter: height'),
    (0x40F184, 'cmp w0, #0x640', cmp_imm(0, W), 'docked test 1 (a width of 1600 meant docked)'),
    (0x40F1AC, 'cmp w0, #0x640', cmp_imm(0, W), 'docked test 2'),
    (0x40F1D0, 'cmp w0, #0x640', cmp_imm(0, W), 'docked test 3'),
]

rows = []
for off, expect, new, why in SITES:
    w = word(off)
    d = dec(w, off)
    got = f'{d.mnemonic} {d.op_str}'
    if got != expect:
        raise SystemExit(f'main+{off:#x}: {got!r} != expected {expect!r}')
    dn = dec(new, off)
    print(f'main+{off:x}: {w:08x} ({got}) -> {new:08x} ({dn.mnemonic} {dn.op_str})  [{why}]')
    rows.append(f'{off:08X} {struct.pack("<I", new).hex()}')

lines = [f'@nsobid-{BUILD_ID}', '',
         f'# Astral Chain internal resolution {W}x{H} (docked base size; the handheld pair and the',
         '# dynamic-resolution arithmetic stay as they are — see scripts/make_res_patch.py)', '@enabled', '']
lines += rows
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text('\n'.join(lines) + '\n')
print(f'wrote {OUT} ({len(rows)} rows)')
