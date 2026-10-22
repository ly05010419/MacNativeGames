#!/usr/bin/env python3
"""Execute BotW 1.9.0's actual floating-point repeat loop with Unicorn.

Usage: python verify_pad_repeat_190.py /path/to/main.patched.bin
Requires unicorn. No game bytes are included. Compare 30/60/120 FPS, quick taps,
release and repress, and the original fast-repeat failure as a negative control.
"""
import math
from pathlib import Path
import re
import struct
import sys
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X19, UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_S0, UC_ARM64_REG_S1, UC_ARM64_REG_CPACR_EL1

START, END, FMOD, CTRL = 0xeb0f78, 0xeb1000, 0x15a05d0, 0x70000000
HELD = 1 << 18
image = Path(sys.argv[1]).read_bytes()
page = (len(image) + 0x3fff) & ~0x3fff
source = (Path(__file__).resolve().parents[2] / 'totk/runtime/game_patches_botw.c').read_text()
body = source.split('static void patch_pad_repeat_190', 1)[1].split('for (size_t', 1)[0]
for offset, word in re.findall(r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', body):
    assert struct.unpack_from('<I', image, int(offset, 16))[0] == int(word, 16), offset


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def float_reg(uc, reg):
    return struct.unpack('<f', struct.pack('<I', uc.reg_read(reg)))[0]


class Loop:
    def __init__(self, patched):
        u = self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        u.mem_map(0, page + 0x4000)
        u.mem_write(0, image)
        u.mem_map(CTRL, 0x1000)
        u.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
        if patched:
            imm = ((page >> 12) - (START >> 12)) & 0x1fffff
            adrp = 0x90000008 | ((imm & 3) << 29) | ((imm >> 2) << 5)
            u.mem_write(START, struct.pack('<I', adrp))
            u.mem_write(0xeb0f80, struct.pack('<I', 0xd503201f))
        c = bytearray(0x140)
        struct.pack_into('<I', c, 0x100, 28)
        for i in range(28):
            c[0xb0+i], c[0xd0+i] = 6, 2
        u.mem_write(CTRL, bytes(c))
        def fmod(uc, address, size, data):
            if address == FMOD:
                uc.reg_write(UC_ARM64_REG_S0, bits(math.fmod(float_reg(uc, UC_ARM64_REG_S0), float_reg(uc, UC_ARM64_REG_S1))))
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))
        u.hook_add(UC_HOOK_CODE, fmod, begin=FMOD, end=FMOD)

    def frame(self, ratio, hold):
        u = self.uc
        u.mem_write(page, struct.pack('<f', ratio))
        u.mem_write(CTRL+0x114, struct.pack('<I', hold))
        u.mem_write(CTRL+0x10, b'\0'*4)
        u.reg_write(UC_ARM64_REG_X19, CTRL)
        u.emu_start(START, END, count=10000)
        assert u.reg_read(UC_ARM64_REG_PC) == END
        return struct.unpack('<I', u.mem_read(CTRL+0x10, 4))[0], struct.unpack('<f', u.mem_read(CTRL+0x30+18*4, 4))[0]


def run(fps, patched, seconds=2):
    loop = Loop(patched)
    times = []
    for frame in range(fps*seconds):
        repeat, count = loop.frame(30/fps, HELD)
        if repeat & HELD:
            times.append((frame+1)/fps)
    return times, count


reference, _ = run(30, False)
for fps in (30, 60, 120):
    times, count = run(fps, True)
    assert len(times) == len(reference), (fps, times, reference)
    assert all(abs(a-b) <= 1/30+1e-6 for a,b in zip(times, reference))
    assert abs(count-60) < 1e-5
    loop = Loop(True)
    for _ in range(round(fps*0.09)):
        repeat, _ = loop.frame(30/fps, HELD)
        assert not repeat, 'quick tap generated a repeat'
    repeat, count = loop.frame(30/fps, 0)
    assert not repeat and count == 0, 'release failed to reset'
    repeat, count = loop.frame(30/fps, HELD)
    assert not repeat and abs(count-30/fps) < 1e-5, 'repress retained stale hold'
    print(f'{fps} FPS: {len(times)} repeats in 2 seconds; quick tap and release/repress passed')
fast, _ = run(120, False)
assert len(fast) > len(reference)*3, 'negative control did not reproduce accelerated repeats'
print(f'Negative control: original loop at 120 FPS repeated {len(fast)} times vs {len(reference)}; regression reproduced')
