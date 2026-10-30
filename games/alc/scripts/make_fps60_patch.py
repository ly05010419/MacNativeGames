#!/usr/bin/env python3
"""Generate the 60 FPS exefs patch for Astral Chain 1.0.1 (mods/1.0.1/60fps/exefs/60fps.pchtxt).

Every site is re-verified against the decompressed main image before it is written: the original
word must decode to the expected instruction, and every replacement word must decode back to the
intended instruction (no hand-built encodings leave this script un-checked).

The mechanism (all offsets are main image offsets, verified here):
  - The game has a native frame-rate mode, n = 0 (60 fps) or 1 (30 fps), consumed two ways:
    the window interval (n == 1 ? 2 : 1, at the nvnWindowBuilderSetPresentInterval call site
    main+0x9e8d84 and the runtime nvnWindowSetPresentInterval site main+0x9db6bc) and per-mode
    timing conversions (main+0x9a0c58: /30 at mode 1, /60 at mode 0 — [0xb5d47c] holds 60.0).
  - The main loop requests mode 1 every frame (main+0x3e39d0). Patch: request 0.
  - The logic step is computed once per frame in the timer main+0x91dbb0:
    step = clamp(measured_ms * [+obj+0xdc] - carry, 1.0, 2.0), in 30 fps frame units. The 1.0
    floor (fmov #1.0 at main+0x91dc94/0x91dca0, mov 0x3f800000 at main+0x91dcac) is what makes
    an interval-1 game run 2x fast: at 60 fps the measured frame is ~16.7 ms = 0.5 steps, and the
    floor rounds every frame up to a full 30 fps step. Patch: floor 0.5.
  - Consumers that truncate the step to whole 30 fps ticks (main+0x48be14 fcvtzs, main+0x57e7e0
    fcvtzu) would read 0 every frame at 60 fps and freeze. The code cave at main+0xa0e7d0 (the
    all-zero assembler padding between .text and .rodata) accumulates the fractional step and
    publishes whole ticks at timer+0xd8; the two consumers read it from there. Timer+0xd8 and
    +0xd4 are free for this: the per-frame flush zeroes both, the deficit carry that otherwise
    owns +0xd4 is inert once the floor is 0.5 (a 60 Hz present can never measure under 16.7 ms),
    and the carry-in add (main+0x91dc14/18) is nopped so the published whole ticks cannot leak
    into the next frame's measured-time sum.

Usage: AC_build/venv/bin/python scripts/make_fps60_patch.py   (from games/alc; writes the pchtxt)
"""
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

HERE = Path(__file__).resolve().parents[1]
IMAGE = HERE / 'AC_build/extracted/v101/main.decompressed.bin'
OUT = HERE / 'mods/1.0.1/60fps/exefs/60fps.pchtxt'
BUILD_ID = '4B159F0F7A360669738390EE8657B4F000000000000000000000000000000000'
CAVE = 0xA0E7D0          # all-zero padding between .text (ends here) and .rodata (0xa0f000)
HOOK = 0x91DCB4          # the timer epilogue's first store: all paths of main+0x91dbb0 pass it

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
img = IMAGE.read_bytes()


def word(off):
    return struct.unpack_from('<I', img, off)[0]


def find_encoding(mnemonic, op_str, at, base, imm_field, imm_values):
    for imm in imm_values:
        w = base | (imm << imm_field)
        try:
            text = next(md.disasm(struct.pack('<I', w), at))
        except StopIteration:
            continue
        if text.mnemonic == mnemonic and text.op_str == op_str:
            return w
    raise SystemExit(f'no encoding found for "{mnemonic} {op_str}"')


def ldr_str(opc_base, rt, rn, off12, scale, at, mnemonic, op):
    w = opc_base | ((off12 // scale) << 10) | (rn << 5) | rt
    text = next(md.disasm(struct.pack('<I', w), at))
    if (text.mnemonic, text.op_str) != (mnemonic, op):
        raise SystemExit(f'encoding {w:08x} decodes to {text.mnemonic} {text.op_str}, expected {mnemonic} {op}')
    return w


def branch(from_, to):
    delta = (to - from_) >> 2
    assert -(1 << 25) <= delta < (1 << 25) and (to - from_) % 4 == 0
    w = 0x14000000 | (delta & 0x3FFFFFF)
    text = next(md.disasm(struct.pack('<I', w), from_))
    assert (text.mnemonic, text.op_str) == ('b', f'#{to:#x}'), text.mnemonic + ' ' + text.op_str
    return w


def check(off, expect_mnemonic, expect_op):
    w = word(off)
    text = next(md.disasm(img[off:off + 4], off))
    got = f'{text.mnemonic} {text.op_str}'
    if got != f'{expect_mnemonic} {expect_op}':
        raise SystemExit(f'main+{off:x}: {got!r} != {expect_mnemonic} {expect_op!r}')
    return w


# (offset, original instruction, replacement word builder, what it does)
FMOV_05 = find_encoding('fmov', 's1, #0.50000000', 0x91DC94, 0x1E201001, 13, range(256))

SITES = []


def site(off, mnemonic, op, new, why):
    SITES.append((off, check(off, mnemonic, op), new, why))


site(0x3E39D0, 'mov', 'w0, #1', 0x52800000, 'main loop requests frame-rate mode 0 (60 fps) every frame')
site(0x9E8D84, 'csinc', 'w1, w8, wzr, ne', 0x52800021, 'startup window interval: mode 1 -> 2 becomes always 1')
site(0x9DB6BC, 'csinc', 'w1, w9, wzr, ne', 0x52800021, 'runtime window interval: mode 1 -> 2 becomes always 1')
site(0x91DC14, 'ldr', 's1, [x19, #0xd8]', 0xD503201F, 'carry-in add (1/2): keep the published whole ticks out of the measured-time sum')
site(0x91DC18, 'fadd', 's0, s0, s1', 0xD503201F, 'carry-in add (2/2)')
site(0x91DC94, 'fmov', 's1, #1.00000000', FMOV_05, 'step floor compare operand 1.0 -> 0.5')
site(0x91DCA0, 'fmov', 's1, #1.00000000', FMOV_05, 'step floor value written back 1.0 -> 0.5 (deficit base)')
site(0x91DCAC, 'mov', 'w8, #0x3f800000', 0x52A7E008, 'floored step 1.0f -> 0.5f')
site(0x91DCB4, 'str', 'x20, [x19, #0xc8]', branch(0x91DCB4, CAVE), 'hook the shared timer epilogue into the whole-tick cave')
site(0x48BE10, 'ldr', 's0, [x8, #0xd0]', 0xB940D908, 'consumer 1 (30-tick trigger): read whole ticks')
site(0x48BE14, 'fcvtzs', 'w8, s0', 0xD503201F, 'consumer 1: no float conversion left to do')
site(0x57E7DC, 'ldr', 's0, [x24, #0xd0]', 0xB940DB08, 'consumer 2 (tick parity): read whole ticks')
site(0x57E7E0, 'fcvtzu', 'w8, s0', 0xD503201F, 'consumer 2: no float conversion left to do')

# every replacement word must decode back to the intended instruction at its site
EXPECTED = {0x3E39D0: ('mov', 'w0, #0'), 0x9E8D84: ('mov', 'w1, #1'), 0x9DB6BC: ('mov', 'w1, #1'),
            0x91DC14: ('nop', ''), 0x91DC18: ('nop', ''),
            0x91DC94: ('fmov', 's1, #0.50000000'), 0x91DCA0: ('fmov', 's1, #0.50000000'), 0x91DCAC: ('mov', 'w8, #0x3f000000'),
            0x91DCB4: ('b', f'#{CAVE:#x}'),
            0x48BE10: ('ldr', 'w8, [x8, #0xd8]'), 0x48BE14: ('nop', ''),
            0x57E7DC: ('ldr', 'w8, [x24, #0xd8]'), 0x57E7E0: ('nop', '')}
for off, _, new, _ in SITES:
    text = next(md.disasm(struct.pack('<I', new), off))
    got = (text.mnemonic, text.op_str)
    if got != EXPECTED[off]:
        raise SystemExit(f'main+{off:x}: replacement {new:08x} decodes to {got}, expected {EXPECTED[off]}')

# the cave: whole-tick accumulator + the displaced epilogue, verbatim. Every hand-built word is
# round-trip checked against capstone before it goes in.
pad = img[CAVE:0xA0F000]
if pad != b'\0' * len(pad):
    raise SystemExit(f'cave site main+{CAVE:x} is not zero padding')
cave_words = [
    ldr_str(0xBD400000, 0, 19, 0xD0, 4, CAVE + 0x00, 'ldr', 's0, [x19, #0xd0]'),   # final step
    ldr_str(0xBD400000, 1, 19, 0xD4, 4, CAVE + 0x04, 'ldr', 's1, [x19, #0xd4]'),   # fractional carry
    0x1E212800,  # fadd  s0, s0, s1
    0x1E380008,  # fcvtzs w8, s0            whole ticks this frame
    0x1E220101,  # scvtf s1, w8
    0x1E213800,  # fsub  s0, s0, s1         new fractional carry
    ldr_str(0xBD000000, 0, 19, 0xD4, 4, CAVE + 0x18, 'str', 's0, [x19, #0xd4]'),
    ldr_str(0xB9000000, 8, 19, 0xD8, 4, CAVE + 0x1C, 'str', 'w8, [x19, #0xd8]'),   # published for the consumers
    word(0x91DCB4),              # str x20, [x19, #0xc8] (displaced by the hook)
] + [word(a) for a in range(0x91DCB8, 0x91DCCC, 4)]  # the rest of the epilogue (…, ret), verbatim
cave = b''.join(struct.pack('<I', w) for w in cave_words)
CAVE_EXPECT = ['ldr s0, [x19, #0xd0]', 'ldr s1, [x19, #0xd4]', 'fadd s0, s0, s1', 'fcvtzs w8, s0',
               'scvtf s1, w8', 'fsub s0, s0, s1', 'str s0, [x19, #0xd4]', 'str w8, [x19, #0xd8]',
               'str x20, [x19, #0xc8]', 'ldp x29, x30, [sp, #0x20]', 'ldp x20, x19, [sp, #0x10]',
               'ldr x21, [sp, #8]', 'ldr d8, [sp], #0x30', 'ret']
assert len(cave_words) == len(CAVE_EXPECT), (len(cave_words), len(CAVE_EXPECT))
for i, (w, expect) in enumerate(zip(cave_words, CAVE_EXPECT)):
    text = next(md.disasm(struct.pack('<I', w), CAVE + 4 * i))
    got = f'{text.mnemonic} {text.op_str}'.strip()
    if got != expect:
        raise SystemExit(f'cave +{4 * i:#x}: {got!r} != {expect!r}')
    print(f'  cave +{4 * i:#04x}: {w:08x}  {got}')

lines = [f'@nsobid-{BUILD_ID}', '', '# Astral Chain 60 FPS: the game\'s native frame-rate mode 0 (interval 1, /60 timing)',
         '# plus a 0.5 logic-step floor and whole-tick consumers. See scripts/make_fps60_patch.py', '@enabled', '']
for off, orig, new, why in SITES:
    print(f'main+{off:x}: {orig:08x} -> {new:08x}  ({why})')
    lines.append(f'{off:08X} {struct.pack("<I", new).hex()}')
lines.append(f'{CAVE:08X} {cave.hex()}  // whole-tick cave')
OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text('\n'.join(lines) + '\n')
print(f'wrote {OUT} ({len(SITES) + 1} rows)')
