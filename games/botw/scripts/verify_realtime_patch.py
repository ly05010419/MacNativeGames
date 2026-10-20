#!/usr/bin/env python3
"""Execute Breath of the Wild 1.6.0's per-frame VFR update (main+0x1c01180) with Unicorn, with and without the real-time
frame-step patch of games/totk/runtime/game_patches_botw.c (botw.md).

A VFR object is laid out the way main+0x183eb98 builds it (interval 2, 1/30 s per frame, every core's pointers), and the
update runs with the game's interval 2. Patched, the frame ratio (+0x3c), core 0's delta frames (+0x108) and delta time
(+0x138) follow the measured frame time written to the data page after main; the unpatched code is the negative control:
it stays at the interval's ratio 1.0 (33.3 ms) whatever the frame time.
usage: verify_realtime_patch.py [main.patched.bin]"""
import json
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_W1, UC_ARM64_REG_X0

ROOT = Path(__file__).resolve().parents[2] / "totk"
image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "rewritten/botw160/main.patched.bin"
image = image_path.read_bytes()
data_off = (len(image) + 0x3fff) & ~0x3fff
FN, RET = 0x1c01180, 0x1c01394
SITES = {0x1c0120c: 0x1e202020, 0x1c01210: 0x3900a008, 0x1c01214: 0x54000060, 0x1c01218: 0xbd003c01}
for offset, original in SITES.items():
    actual, = struct.unpack_from("<I", image, offset)
    if actual != original:
        raise SystemExit(f"original instruction mismatch at main+{offset:#x}: {actual:08x} != {original:08x}")
ret, = struct.unpack_from("<I", image, RET)
if ret != 0xd65f03c0:
    raise SystemExit(f"main+{RET:#x} is {ret:08x}, not the update's ret")


def adrp(rd, site, target):
    pages = (target >> 12) - (site >> 12)
    assert -(1 << 20) <= pages < (1 << 20)
    imm = pages & 0x1fffff
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


PATCH = {0x1c0120c: adrp(9, 0x1c0120c, data_off), 0x1c01214: 0xbd400121}   # adrp x9, <page>; ldr s1, [x9]
VFR = 0x100000


def vfr_object():
    o = bytearray(0x160)
    struct.pack_into("<I", o, 0x2c, 1)
    struct.pack_into("<f", o, 0x30, 1.0)
    struct.pack_into("<II", o, 0x34, 2, 2)          # interval, previous interval (second setup at main+0x183ec94)
    struct.pack_into("<ff", o, 0x3c, 1.0, 1.0)      # ratio, previous ratio
    struct.pack_into("<I", o, 0x88, 0x59)
    struct.pack_into("<IIf", o, 0x150, 2, 30, 1 / 30)
    for i in range(3):                              # per-core pointer arrays, as main+0x183ec3c..0x183ec8c set them
        struct.pack_into("<Q", o, 0x48 + 8 * i, VFR + 0x34)
        struct.pack_into("<Q", o, 0x60 + 8 * i, VFR + 0x3c)
        struct.pack_into("<Q", o, 0xf0 + 8 * i, VFR + 0x8c + 4 * i)
        struct.pack_into("<Q", o, 0x108 + 8 * i, VFR + 0x98 + 4 * i)
        struct.pack_into("<Q", o, 0x120 + 8 * i, VFR + 0xa4 + 4 * i)
        struct.pack_into("<Q", o, 0x138 + 8 * i, VFR + 0xb0 + 4 * i)
    return bytes(o)


def run(seconds, patched, frames=3):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for page in sorted({FN & ~0x3fff, RET & ~0x3fff, 0x260d000 & ~0x3fff}):
        mu.mem_map(page, 0x4000)
        mu.mem_write(page, image[page:page + 0x4000])
    mu.mem_map(data_off, 0x4000)
    mu.mem_map(VFR, 0x4000)
    if patched:
        for offset, word in PATCH.items():
            mu.mem_write(offset, struct.pack("<I", word))
    mu.mem_write(VFR, vfr_object())
    mu.mem_write(data_off, struct.pack("<ff", seconds * 30, seconds))
    for _ in range(frames):                         # several frames: the ratio must be written every frame, not only on a change
        mu.reg_write(UC_ARM64_REG_X0, VFR)
        mu.reg_write(UC_ARM64_REG_W1, 2)
        mu.reg_write(UC_ARM64_REG_SP, VFR + 0x3000)
        mu.emu_start(FN, RET, count=400)
        assert mu.reg_read(UC_ARM64_REG_PC) == RET
    o = mu.mem_read(VFR, 0x160)
    ratio, = struct.unpack_from("<f", o, 0x3c)
    frames_core0, = struct.unpack_from("<f", o, 0x98)
    seconds_core0, = struct.unpack_from("<f", o, 0xb0)
    interval, = struct.unpack_from("<I", o, 0x34)
    return ratio, frames_core0, seconds_core0, interval


# The fade at main+0x1821548: from the load of the VFR's interval pointer to the step (s0) before it is added.
FADE, FADE_END = 0x1821564, 0x1821590
FADE_ORIG = {0x1821564: 0xf9400108, 0x1821568: 0xb9400108, 0x182156c: 0x7100011f, 0x1821570: 0x1a9f1508,
             0x1821574: 0x1e230100, 0x1821578: 0x1e2c100b, 0x182157c: 0xb0006f68, 0x1821580: 0xbd409109,
             0x1821584: 0xbd435ee1, 0x1821588: 0x1e2b0800, 0x182158c: 0x1e290800}
for offset, original in FADE_ORIG.items():
    actual, = struct.unpack_from("<I", image, offset)
    if actual != original:
        raise SystemExit(f"original instruction mismatch at main+{offset:#x}: {actual:08x} != {original:08x}")
NOP = 0xd503201f
FADE_PATCH = {0x1821568: adrp(8, 0x1821568, data_off), 0x182156c: 0xbd400100, 0x1821570: NOP, 0x1821574: NOP, 0x1821588: NOP}


def fade_step(seconds, patched, interval=2):
    from unicorn.arm64_const import UC_ARM64_REG_X8, UC_ARM64_REG_X23, UC_ARM64_REG_S0
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    for page in sorted({FADE & ~0x3fff, 0x260e000 & ~0x3fff}):
        mu.mem_map(page, 0x4000)
        mu.mem_write(page, image[page:page + 0x4000])
    mu.mem_map(data_off, 0x4000)
    mu.mem_map(VFR, 0x4000)
    if patched:
        for offset, word in FADE_PATCH.items():
            mu.mem_write(offset, struct.pack("<I", word))
    mu.mem_write(data_off, struct.pack("<ff", seconds * 30, seconds))
    mu.mem_write(VFR + 0x34, struct.pack("<I", interval))      # the interval the per-core pointer points at
    mu.mem_write(VFR + 0x48, struct.pack("<Q", VFR + 0x34))   # x8 = &pointer (the core's slot of the +0x48 array)
    mu.reg_write(UC_ARM64_REG_X8, VFR + 0x48)
    mu.reg_write(UC_ARM64_REG_X23, VFR + 0x1000)               # the fading object; +0x35c = 0.0
    mu.emu_start(FADE, FADE_END, count=20)
    assert mu.reg_read(UC_ARM64_REG_PC) == FADE_END
    return struct.unpack("<f", struct.pack("<I", mu.reg_read(UC_ARM64_REG_S0) & 0xffffffff))[0]


checks = []
for hz in (30, 45, 60, 90, 100, 120):
    s = 1 / hz
    ratio, df, dt, interval = run(s, True)
    assert abs(ratio - s * 30) < 1e-6, (hz, ratio)
    assert abs(df - s * 30) < 1e-6, (hz, df)
    assert abs(dt - s) < 1e-7, (hz, dt)
    assert interval == 2
    fade = fade_step(s, True)
    assert abs(fade - s * 30 * 0.2) < 1e-6, (hz, fade)
    checks.append({"hz": hz, "ratio": ratio, "delta_frames": df, "delta_time_ms": dt * 1e3, "fade_step": fade})
assert abs(fade_step(1 / 30, False) - 0.2) < 1e-6   # shipped: interval 2 x 0.5 x 0.2, the patched value at 30 fps
negative = run(1 / 120, False)
assert abs(negative[0] - 1.0) < 1e-6 and abs(negative[2] - 1 / 30) < 1e-7, negative
negative_fade = fade_step(1 / 120, False)
assert abs(negative_fade - 0.2) < 1e-6, negative_fade
print(json.dumps({"status": "pass", "image": str(image_path), "data_page": hex(data_off), "patched": checks,
                  "negative_control_at_120hz": {"ratio": negative[0], "delta_time_ms": negative[2] * 1e3,
                                                "fade_step": negative_fade}}, indent=2))
