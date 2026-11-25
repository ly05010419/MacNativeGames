#!/usr/bin/env python3
"""Execute Breath of the Wild 1.9.0's arrow fall update (main+0x5ee6f0..0x5ee744: the last velocity normalized, x the speed,
plus the gravity vector) with Unicorn, with and without patch_arrow_gravity of games/totk/runtime/game_patches_botw.c.

Why it matters: an arrow must turn down by the same angle per second of game time at any frame rate, or above 30 fps it
falls short (Savage Wind Recomp #144). An arrow is shot level and updated for one second at 30, 60, 120, 144 and 240 fps
(the frame-step page holding each rate's ratio), each update running the game's own instructions; its pitch and its drop
after that second are compared with 30 fps. Patched they match 30 fps; the shipped code is the negative control: it turns
about rate / 30 times as fast. The check[] and veneer[] words and the veneer's place are read from the C source, nothing
about them is restated here.
usage: verify_arrow_gravity_patch.py [main.patched.bin]   (default totk/rewritten/botw190/main.patched.bin)"""
import argparse
import json
import math
import re
import struct
import sys
from pathlib import Path

from unicorn import arm64_const as A
from unicorn.arm64_const import UC_ARM64_REG_PC

sys.path.insert(0, str(Path(__file__).resolve().parent))
from botw190_unicorn import ROOT, VFR, adrp, f32, machine, sreg

TEXT_END = 0x15a1c64                      # 1.9.0 main's text end (verify_kf_angle_patch.py)
START, END = 0x5ee6f0, 0x5ee744           # from the last velocity's length to the call after the sum
ARROW = VFR + 0x10000
SPEED, GRAVITY = 2.0, -0.005              # metres per console frame: ~4 degrees a second at 30 fps, so that even the
                                          # shipped 240 fps (8x) stays where the turn grows about linearly. In game
                                          # (TOTK_ARROW_LOG=1) a bow shot had speed 1.0 and gravity -0.0109: ~18.6 a second,
                                          # which shipped at 240 fps would turn past the vertical within the second


def b(site, target):
    return 0x14000000 | (((target - site) >> 2) & 0x3ffffff)


def c_tables(data_off):
    """check {off: word}, and the words patch_arrow_gravity writes {off: word} with its veneer at the offset patch_realtime
    hands it."""
    src = (ROOT / "runtime/game_patches_botw.c").read_text().split("Breath of the Wild 1.9.0", 1)[1]
    body = src[src.index("static void patch_arrow_gravity("):]
    body = body[:body.index("\n}\n")]
    site, back = (int(x, 16) for x in re.search(r"SITE = 0x([0-9a-f]+), BACK = 0x([0-9a-f]+)", body).groups())
    table = body.split("check[] = {", 1)[1].split("};", 1)[0]
    check = {int(o, 16): int(w, 16) for o, w in re.findall(r"\{0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u\}", table)}
    check.update({off: int(w, 16) for name, w in re.findall(r"\{(SITE|BACK), 0x([0-9a-f]+)u\}", table)
                  for off in [site if name == "SITE" else back]})
    assert len(check) == table.count("{"), "a check[] entry of patch_arrow_gravity is in a form this script cannot read"
    plus = int(re.search(r"patch_arrow_gravity\(m, page, \(\(m->text_end \+ 15\) & ~15ULL\) \+ 0x([0-9a-f]+)\)", src).group(1), 16)
    stub = ((TEXT_END + 15) & ~15) + plus
    words = []
    for line in body.split("veneer[] = {", 1)[1].split("};", 1)[0].splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).strip().rstrip(",")
        if not line:
            continue
        at = stub + 4 * len(words)
        if m := re.fullmatch(r"0x([0-9a-f]{8})u", line):
            words.append(int(m.group(1), 16))
        elif m := re.fullmatch(r"adrp_reg\((\d+), \(uintptr_t\)m->base \+ stub \+ (\d+), page\)", line):
            assert stub + int(m.group(2)) == at, line
            words.append(adrp(int(m.group(1)), at, data_off))
        elif m := re.fullmatch(r"branch_to\(stub \+ (\d+), BACK\)", line):
            assert stub + int(m.group(1)) == at, line
            words.append(b(at, back))
        else:
            raise SystemExit(f"unrecognised veneer line: {line}")
    put = {stub + 4 * i: w for i, w in enumerate(words)}
    put[site] = b(site, stub)
    return check, put, stub, len(words)


def fly(image, data_off, patches, hz):
    """One second of game time at hz: the arrow's pitch (degrees) and its drop (metres, the body moving at velocity x 30
    for 1 / hz each update), from a level shot."""
    mu = machine(image, data_off, patches)
    ratio = f32(30 / hz)
    mu.mem_write(data_off, struct.pack("<ff", ratio, 1 / hz))
    arrow = bytearray(0x120)
    struct.pack_into("<f", arrow, 0xb4, SPEED)
    struct.pack_into("<fff", arrow, 0x104, 0.0, GRAVITY, 0.0)
    mu.mem_write(ARROW, bytes(arrow))
    v, drop = (SPEED, 0.0, 0.0), 0.0
    for _ in range(hz):
        mu.reg_write(A.UC_ARM64_REG_X20, ARROW)
        for reg, x in zip((A.UC_ARM64_REG_S8, A.UC_ARM64_REG_S9, A.UC_ARM64_REG_S10), v):
            mu.reg_write(reg, struct.unpack("<I", struct.pack("<f", x))[0])
        mu.emu_start(START, END, count=100)
        assert mu.reg_read(UC_ARM64_REG_PC) == END
        v = tuple(sreg(mu, r) for r in (A.UC_ARM64_REG_S8, A.UC_ARM64_REG_S9, A.UC_ARM64_REG_S10))
        drop += v[1] * 30 / hz
    return math.degrees(math.atan2(v[1], math.hypot(v[0], v[2]))), drop


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", nargs="?", type=Path, default=ROOT / "rewritten/botw190/main.patched.bin")
    image = ap.parse_args().image.read_bytes()
    data_off = (len(image) + 0x3fff) & ~0x3fff
    check, put, stub, n = c_tables(data_off)
    for off, w in check.items():
        have, = struct.unpack_from("<I", image, off)
        if have != w:
            raise SystemExit(f"original instruction mismatch at main+{off:#x}: {have:08x} != {w:08x}")
    if stub + 4 * n > ((TEXT_END + 0xfff) & ~0xfff) or any(image[stub:stub + 4 * n]):
        raise SystemExit("the veneer does not fit the empty padding after the text")
    ref = fly(image, data_off, put, 30)
    assert ref == fly(image, data_off, {}, 30), "at 30 fps the patched arrow must fly as shipped"
    rows = []
    for hz in (30, 60, 120, 144, 240):
        pitch, drop = fly(image, data_off, put, hz)
        shipped_pitch, shipped_drop = fly(image, data_off, {}, hz)
        # patched: within 3% of 30 fps (only the steps' discretization differs); shipped: turning about hz / 30 as fast
        assert abs(pitch / ref[0] - 1) < 0.03 and abs(drop / ref[1] - 1) < 0.03, (hz, pitch, drop, ref)
        if hz > 30:
            assert shipped_pitch / ref[0] > 0.8 * hz / 30 and shipped_drop / ref[1] > 0.8 * hz / 30, (hz, shipped_pitch, shipped_drop, ref)
        rows.append({"hz": hz, "pitch_deg": round(pitch, 3), "drop_m": round(drop, 3),
                     "shipped_pitch_deg": round(shipped_pitch, 3), "shipped_drop_m": round(shipped_drop, 3)})
    print(json.dumps({"status": "pass", "version": "botw190", "veneer": hex(stub), "guarded_words": len(check),
                      "speed": SPEED, "gravity": GRAVITY, "one_second": rows}, indent=2))


if __name__ == "__main__":
    main()
