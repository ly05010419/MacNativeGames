#!/usr/bin/env python3
"""Execute Breath of the Wild 1.9.0's sound-frame select (main+0x1551004..0x1551010: the VFR's delta frame, at least a
floor, into s8, which steps the audio frame and is stored at +0x84 of the sound system) with Unicorn, with and without
patch_sound_frame of games/totk/runtime/game_patches_botw.c.

Why it matters: the sound's clock must advance one second per second of game time at any frame rate, and as shipped it
does not above 60 fps, where the floor (a 60 fps frame) sets every frame: x2 at 120 fps, and the field battle music
plays its segments over each other (Savage Wind Recomp #145). Where the delta itself is below the floor (slow motion),
the console's 30 fps behaviour must stay: half a second of sound clock a second.
Checked at 30, 60, 120, 144 and 240 fps (the frame-step page holding each rate's ratio), each frame running the game's
own instructions:
  - normal speed: patched, the sound's clock runs at 1.0x at every rate; shipped (the negative control) runs fast above 60;
  - slow motion and a stopped delta: patched, the sound's clock per second equals shipped at 30 fps;
  - a page ratio of exactly 1.0: patched gives the same bits as shipped, for any delta.
The check[] and veneer[] words and the veneer's place are read from the C source, nothing about them is restated here.
usage: verify_sound_frame_patch.py [main.patched.bin]   (default totk/rewritten/botw190/main.patched.bin)"""
import argparse
import json
import re
import struct
import sys
from pathlib import Path

from unicorn import arm64_const as A
from unicorn.arm64_const import UC_ARM64_REG_PC

sys.path.insert(0, str(Path(__file__).resolve().parent))
from botw190_unicorn import ROOT, adrp, f32, machine, sreg

TEXT_END = 0x15a1c64                      # 1.9.0 main's text end (verify_kf_angle_patch.py)
START, END = 0x1551004, 0x1551010         # from the floor's load to the call after the select
SLOW = (0.3, 0.1, 0.0)                    # the delta as a share of the frame: slow motion, and a stopped game


def b(site, target):
    return 0x14000000 | (((target - site) >> 2) & 0x3ffffff)


def c_tables(data_off):
    """check {off: word}, and the words patch_sound_frame writes {off: word} with its veneer at the offset patch_realtime
    hands it."""
    src = (ROOT / "runtime/game_patches_botw.c").read_text().split("Breath of the Wild 1.9.0", 1)[1]
    body = src[src.index("static void patch_sound_frame("):]
    body = body[:body.index("\n}\n")]
    site, back = (int(x, 16) for x in re.search(r"SITE = 0x([0-9a-f]+), BACK = 0x([0-9a-f]+)", body).groups())
    table = body.split("check[] = {", 1)[1].split("};", 1)[0]
    check = {int(o, 16): int(w, 16) for o, w in re.findall(r"\{0x([0-9a-f]+)u,\s*0x([0-9a-f]+)u\}", table)}
    check.update({off: int(w, 16) for name, w in re.findall(r"\{(SITE|BACK), 0x([0-9a-f]+)u\}", table)
                  for off in [site if name == "SITE" else back]})
    assert len(check) == table.count("{"), "a check[] entry of patch_sound_frame is in a form this script cannot read"
    plus = int(re.search(r"patch_sound_frame\(m, page, \(\(m->text_end \+ 15\) & ~15ULL\) \+ 0x([0-9a-f]+)\)", src).group(1), 16)
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


class Frame:
    """One machine per image and patch set; each call runs the select once for a page ratio and a delta."""

    def __init__(self, image, data_off, patches):
        self.mu, self.data_off = machine(image, data_off, patches), data_off

    def __call__(self, ratio, delta):
        mu = self.mu
        mu.mem_write(self.data_off, struct.pack("<ff", f32(ratio), ratio / 30))
        mu.reg_write(A.UC_ARM64_REG_S0, struct.unpack("<I", struct.pack("<f", delta))[0])
        mu.reg_write(A.UC_ARM64_REG_X8, 0)
        mu.emu_start(START, END, count=20)
        assert mu.reg_read(UC_ARM64_REG_PC) == END
        assert sreg(mu, A.UC_ARM64_REG_S0) == f32(delta), "the delta must reach the select unchanged"
        return sreg(mu, A.UC_ARM64_REG_S8)


def clock(frame, hz, share):
    """Seconds of sound clock in one second at hz, the delta being share x the frame (console frames: / 30)."""
    ratio = 30 / hz
    return sum(frame(ratio, f32(ratio * share)) for _ in range(hz)) / 30


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
    patched, shipped = Frame(image, data_off, put), Frame(image, data_off, {})
    # at 30 fps (ratio 1.0) the patched select is the shipped one, bit for bit, whatever the delta
    for delta in (0.0, 0.1, 0.25, 0.4999, 0.5, 0.5001, 0.75, 1.0, 1.5, 2.0, 3.0):
        assert patched(1.0, delta) == shipped(1.0, delta), delta
    slow30 = {share: clock(shipped, 30, share) for share in SLOW}
    rows = []
    for hz in (30, 60, 120, 144, 240):
        normal, normal_shipped = clock(patched, hz, 1.0), clock(shipped, hz, 1.0)
        assert abs(normal - 1) < 1e-3, (hz, normal)                       # one second of sound a second
        if hz > 60:                                                        # shipped: the floor sets every frame
            assert abs(normal_shipped - hz / 60) < 1e-3, (hz, normal_shipped)
        slow = {share: clock(patched, hz, share) for share in SLOW}
        for share in SLOW:                                                 # the console's floor, by real time
            assert abs(slow[share] - slow30[share]) < 1e-3, (hz, share, slow[share], slow30[share])
        rows.append({"hz": hz, "sound_s_per_s": round(normal, 4), "shipped_sound_s_per_s": round(normal_shipped, 4),
                     "slow_motion": {str(s): round(slow[s], 4) for s in SLOW},
                     "shipped_slow_motion": {str(s): round(clock(shipped, hz, s), 4) for s in SLOW}})
    print(json.dumps({"status": "pass", "version": "botw190", "veneer": hex(stub), "guarded_words": len(check),
                      "console_30fps_slow_motion": {str(s): round(v, 4) for s, v in slow30.items()}, "one_second": rows},
                     indent=2))


if __name__ == "__main__":
    main()
