#!/usr/bin/env python3
"""Run the button loop of Breath of the Wild 1.6.0's sead::ControllerBase::updateDerivativeParams_ (main+0x126e490..
0x126e528) with Unicorn, as shipped and with the pad-repeat patch of games/totk/runtime/game_patches_botw.c (botw.md).

The loop counts the frames each button has been held and raises its repeat bit at the delay and then every pulse frames.
A controller is laid out the way its constructor (main+0x126e100) builds it, with the menus' delay 6 / pulse 2 on the
stick and d-pad directions (main+0x1a40958). A direction is held for 2 s at 30, 60, 95 and 120 fps:
- shipped at 30 fps is the reference (repeats at 0.2 s and then every 1/15 s);
- shipped at 120 fps is the bug (four times as fast) and must not match the reference;
- patched, with the host's tick (one per 1/30 s of game time, games/totk/runtime/game_patches_botw.c pad_tick_advance),
  every repeat must come within one 1/30 s tick of the reference at every frame rate and from every starting phase;
- patched with a tick on every frame at 30 fps must equal the shipped loop exactly (repeat bits and counts, frame by frame);
- released buttons reset their count on any frame, with or without a tick;
- negative control: the patched loop without its tick test (cbz w16 -> nop) runs four times as fast at 120 fps again.
Then the pause menu's fast-repeat countdown (main+0x1214474, the record {mask, 60, countdown, 9, 3, 60, 1}): shipped it
takes one off per frame and at zero branches on to the fast switch; patched it takes one off only on a tick, falls
through to the switch exactly once, and leaves a record alone on frames without a tick.
The patched words are read from game_patches_botw.c (pad_loop_ticked, menu_cd_ticked), so this checks what the runtime writes.
usage: verify_pad_repeat_patch.py [main.patched.bin]"""
import re
import struct
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import UC_ARM64_REG_PC, UC_ARM64_REG_X11, UC_ARM64_REG_X19

ROOT = Path(__file__).resolve().parents[2] / "totk"
image_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "rewritten/botw160/main.patched.bin"
image = image_path.read_bytes()
data_off = (len(image) + 0x3fff) & ~0x3fff
LOOP, END, WORDS, ADRP = 0x126e490, 0x126e528, 38, 7
CTRL = 0x7000_0000
STICKS = 0xff0000                     # the directions the menus set to delay 6 / pulse 2
HELD = 1 << 18                        # one of them

src = (ROOT / "runtime/game_patches_botw.c").read_text()


def c_words(name, n=WORDS):
    body = re.search(r"static const uint32_t %s\[\w+\] = \{(.*?)\};" % name, src, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    words = [int(w.rstrip("u"), 16) if w.startswith("0x") else int(w) for w in re.findall(r"0x[0-9a-fA-F]+u?|\b0\b", body)]
    if len(words) != n:
        raise SystemExit(f"{name}: {len(words)} words in game_patches_botw.c, expected {n}")
    return words


def adrp(rd, site, target):
    pages = (target >> 12) - (site >> 12)
    imm = pages & 0x1fffff
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd


shipped = c_words("pad_loop_shipped")
ticked = c_words("pad_loop_ticked")
ticked[ADRP] = adrp(16, LOOP + 4 * ADRP, data_off)
for i in range(WORDS):
    actual, = struct.unpack_from("<I", image, LOOP + 4 * i)
    if actual != shipped[i]:
        raise SystemExit(f"main+{LOOP + 4 * i:#x} is {actual:08x}, not the shipped {shipped[i]:08x}")
no_tick_test = list(ticked)
no_tick_test[ticked.index(0x34000290)] = 0xd503201f          # cbz w16, next -> nop


class Loop:
    def __init__(self, words):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        size = data_off + 0x4000
        self.uc.mem_map(0, size)
        self.uc.mem_write(0, image)
        self.uc.mem_write(LOOP, struct.pack("<%dI" % WORDS, *words))
        self.uc.mem_map(CTRL, 0x1000)
        c = bytearray(0x140)
        struct.pack_into("<i", c, 0x100, 0x1c)                 # buttons, as the constructor sets
        for i in range(32):
            c[0xb0 + i], c[0xd0 + i] = (6, 2) if STICKS >> i & 1 else (30, 1)
        self.uc.mem_write(CTRL, bytes(c))

    def frame(self, hold, tick):
        self.uc.mem_write(data_off + 8, struct.pack("<I", tick))
        self.uc.reg_write(UC_ARM64_REG_X19, CTRL)
        self.uc.reg_write(UC_ARM64_REG_X11, hold)
        self.uc.emu_start(LOOP, END, count=10000)
        if self.uc.reg_read(UC_ARM64_REG_PC) != END:
            raise SystemExit("the loop did not reach its end")
        repeat, = struct.unpack("<I", self.uc.mem_read(CTRL + 0x10, 4))
        counts = struct.unpack("<32I", self.uc.mem_read(CTRL + 0x30, 128))
        return repeat, counts


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def run(words, fps, seconds=2.0, phase=0.0, ticks="host", release_at=None):
    """Hold HELD from t = 0 for `seconds`; returns the times of its repeats and every frame's (repeat, counts)."""
    loop, step = Loop(words), 1.0 / fps
    pad_phase, times, trace = phase, [], []
    for k in range(int(round(seconds * fps))):
        if ticks == "host":                                    # pad_tick_advance: the next frame's step on the page
            pad_phase += f32(step) * 30.0
            tick = 0
            if pad_phase >= 1.0:
                tick, pad_phase = 1, pad_phase - 1.0
                if pad_phase >= 1.0:
                    pad_phase = 0.0
        else:
            tick = ticks
        hold = HELD if release_at is None or k * step < release_at else 0
        repeat, counts = loop.frame(hold, tick)
        trace.append((repeat, counts))
        if repeat & HELD:
            times.append(k * step)
    return times, trace


failures = []


def check(ok, what):
    print(("ok    " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


ref, ref_trace = run(shipped, 30, ticks=1)
check(abs(ref[0] - 0.2) < 1e-9 and all(abs(b - a - 1 / 15) < 1e-9 for a, b in zip(ref, ref[1:])),
      f"shipped at 30 fps: first repeat {ref[0]:.3f} s, then every {ref[1] - ref[0]:.4f} s, {len(ref)} in 2 s")
fast, _ = run(shipped, 120, ticks=1)
check(len(fast) > 3 * len(ref), f"shipped at 120 fps (the bug): {len(fast)} repeats in 2 s, first at {fast[0]:.3f} s")

for fps in (30, 60, 95, 120):
    worst, counts = 0.0, set()
    for phase in (0.0, 0.25, 0.5, 0.75, 0.999):
        got, _ = run(ticked, fps, phase=phase)
        counts.add(len(got))
        n = min(len(got), len(ref))
        worst = max([worst] + [abs(a - b) for a, b in zip(got[:n], ref[:n])])
    check(worst < 1 / 30 + 1 / fps and all(abs(c - len(ref)) <= 1 for c in counts),
          f"patched at {fps} fps: {sorted(counts)} repeats in 2 s (reference {len(ref)}), worst offset {worst * 1e3:.1f} ms")

same = run(ticked, 30, ticks=1)[1] == ref_trace
check(same, "patched with a tick on every frame equals the shipped loop frame by frame (repeat bits and counts)")

_, frozen = run(ticked, 120, seconds=0.5, ticks=0)
check(all(r == 0 and c[18] == 0 for r, c in frozen), "patched without ticks: no repeat and the count stays 0")

_, rel = run(ticked, 120, seconds=1.0, release_at=0.5)    # frame 60 releases; at 120 fps from phase 0 it has no tick
check(rel[59][1][18] > 0 and rel[60][1][18] == 0 and rel[61][1][18] == 0,
      f"patched: a release on a frame without a tick resets the count at once (count {rel[59][1][18]} -> {rel[60][1][18]})")

neg, _ = run(no_tick_test, 120)
check(len(neg) > 3 * len(ref), f"negative control, the tick test removed: {len(neg)} repeats in 2 s at 120 fps (the bug again)")

# the pause menu's countdown
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X10, UC_ARM64_REG_X27
CD, CD_DONE, CD_SWITCH, CD_WORDS, CD_ADRP = 0x1214474, 0x12145e0, 0x1214498, 9, 2
cd_shipped, cd_ticked = c_words("menu_cd_shipped", CD_WORDS), c_words("menu_cd_ticked", CD_WORDS)
cd_ticked[CD_ADRP] = adrp(13, CD + 4 * CD_ADRP, data_off)
for i in range(CD_WORDS):
    actual, = struct.unpack_from("<I", image, CD + 4 * i)
    if actual != cd_shipped[i]:
        raise SystemExit(f"main+{CD + 4 * i:#x} is {actual:08x}, not the shipped {cd_shipped[i]:08x}")
MENU = 0x7100_0000                                         # the menu object x27; its records at +0x649f8 + 16 i


class Countdown:
    def __init__(self, words):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(0, data_off + 0x4000)
        self.uc.mem_write(0, image)
        self.uc.mem_write(CD, struct.pack("<%dI" % CD_WORDS, *words))
        self.uc.mem_map(MENU, 0x70000)
        rec = struct.pack("<IIIBBBB", 0xf0000, 60, 60, 9, 3, 60, 1)   # as main+0x1207d0c builds record 0
        self.uc.mem_write(MENU + 0x649f8, rec)
        self.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, d: uc.emu_stop() if a == CD_SWITCH else None)

    def frame(self, tick):
        """Runs the block for record 0; returns (countdown after, reached the fast switch)."""
        self.uc.mem_write(data_off + 8, struct.pack("<I", tick))
        self.uc.reg_write(UC_ARM64_REG_X27, MENU)
        self.uc.reg_write(UC_ARM64_REG_X10, 0)
        self.uc.emu_start(CD, CD_DONE, count=20)
        pc = self.uc.reg_read(UC_ARM64_REG_PC)
        if pc not in (CD_DONE, CD_SWITCH):
            raise SystemExit(f"the countdown block stopped at {pc:#x}")
        value, = struct.unpack("<i", self.uc.mem_read(MENU + 0x64a00, 4))
        return value, pc == CD_SWITCH


def countdown_run(words, ticks):
    cd, out = Countdown(words), []
    for t in ticks:
        out.append(cd.frame(t))
    return out


ship = countdown_run(cd_shipped, [1] * 62)
check([v for v, _ in ship[:61]] == list(range(59, -2, -1)) and [sw for _, sw in ship].count(True) == 1 and ship[60][1],
      "shipped countdown: one off per frame, the fast switch on the frame it reaches zero, then nothing")
pat = countdown_run(cd_ticked, [1] * 62)
check(pat[:61] == ship[:61] and pat[61] == (-2, False),
      "patched countdown with a tick every frame equals the shipped block up to the switch; after it the value keeps falling (-2), no second switch")
pat2 = countdown_run(cd_ticked, [1, 0, 0, 0] * 62)
check([v for v, _ in pat2[::4]][:61] == [v for v, _ in ship[:61]] and [sw for _, sw in pat2].count(True) == 1 and pat2[240][1]
      and all(not sw and v == pat2[i - i % 4][0] for i, (v, sw) in enumerate(pat2) if i % 4),
      "patched countdown at 120 fps (a tick every 4th frame): frames without a tick leave the record alone, one off per tick, the one switch on the 61st tick")
neg = countdown_run(cd_shipped, [1, 0, 0, 0] * 62)
check(neg[60][1] and [v for v, _ in neg[:61]] == list(range(59, -2, -1)), "negative control: the shipped block ignores the tick and switches after 60 frames")

if failures:
    raise SystemExit(f"{len(failures)} check(s) failed")
print("pad repeat patch: all checks passed")
