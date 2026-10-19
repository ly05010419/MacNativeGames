#!/usr/bin/env python3
"""Run the two real TOTK atomic windows whose external exits were misordered.

The success path executes original LDXR/STXR instructions without deexcl.
The failure path injects one failed-store outcome before the status branch;
this checks branch/state semantics, not concurrent exclusive-monitor behavior.
"""
import argparse
import json
import random
import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'third_party/d2r'))
import nsoimg

IB, ARENA, TSD, SP, CELL = 0x100000000, 0x400000, 0x2000000, 0x410000, 0x402000
GP = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]
VR = [UC_ARM64_REG_Q0 + i for i in range(32)]


def target(word, address):
    return address + 4 * (((word & 0x3FFFFFF) ^ 0x2000000) - 0x2000000)


def run(original, patched, header, start, failure, seed, use_patch):
    rng = random.Random(seed)
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    codepage = start & ~4095
    u.mem_map(IB + codepage, 8192)
    u.mem_write(IB + codepage, bytes((patched if use_patch else original)[codepage:codepage + 8192]))
    trampoline = target(struct.unpack_from('<I', patched, start)[0], start)
    pp = trampoline & ~4095
    if use_patch:
        u.mem_map(IB + pp, 8192)
        u.mem_write(IB + pp, bytes(patched[pp:pp + 8192]))
        kp = header['KEY_PAGE_OFF']
        u.mem_map(IB + kp, 16384)
        u.mem_write(IB + kp + 8 * header['KEY_K18_IDX'], struct.pack('<Q', 300 * 8))
    u.mem_map(ARENA, 0x20000)
    u.mem_write(ARENA, rng.randbytes(0x20000))
    u.mem_map(TSD, 0x10000)
    g = [rng.getrandbits(64) for _ in range(31)]
    g[16] = CELL
    initial = rng.randrange(2, 1 << 31)
    u.mem_write(CELL, struct.pack('<I', initial))
    for r, v in zip(GP, g): u.reg_write(r, v)
    for r in VR: u.reg_write(r, rng.getrandbits(128))
    u.reg_write(UC_ARM64_REG_SP, SP)
    u.reg_write(UC_ARM64_REG_NZCV, rng.randrange(16) << 28)
    u.reg_write(UC_ARM64_REG_TPIDRRO_EL0, TSD)
    u.mem_write(TSD + 300 * 8, struct.pack('<Q', g[18]))
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    source = patched if use_patch else original
    low = trampoline if use_patch else start
    instructions = list(md.disasm(bytes(source[low:low + (128 if use_patch else 16)]), low))
    store = next(i for i in instructions if i.mnemonic == 'stxr')
    status_register = (struct.unpack('<I', store.bytes)[0] >> 16) & 31
    branch = next(md.disasm(original[start + 12:start + 16], start + 12))
    expected = IB + (start + 16 if failure else branch.operands[-1].imm)
    seen_patch, stopped = False, False

    def hook(uc, address, size, _):
        nonlocal seen_patch, stopped
        if use_patch and IB + pp <= address < IB + pp + 8192:
            seen_patch = True
        if address == IB + store.address + 4 and failure:
            uc.reg_write(GP[status_register], 1)
            uc.mem_write(CELL, struct.pack('<I', initial))
        exit_window = seen_patch and address < IB + header['PATCH_OFF'] if use_patch else not IB + start <= address < IB + start + 16
        if exit_window:
            stopped = True
            uc.emu_stop()

    u.hook_add(UC_HOOK_CODE, hook)
    u.emu_start(IB + start, IB + 0x8000000, count=200)
    assert stopped and u.reg_read(UC_ARM64_REG_PC) == expected, (hex(start), failure, hex(u.reg_read(UC_ARM64_REG_PC)), hex(expected))
    state = [u.reg_read(r) for r in GP]
    if use_patch: state[18] = struct.unpack('<Q', u.mem_read(TSD + 300 * 8, 8))[0]
    assert state[18] == int(failure)
    assert struct.unpack('<I', u.mem_read(CELL, 4))[0] == initial - (not failure)
    return (state, u.reg_read(UC_ARM64_REG_SP), u.reg_read(UC_ARM64_REG_NZCV),
            [u.reg_read(r) for r in VR], bytes(u.mem_read(CELL, 8)),
            bytes(u.mem_read(SP, 128)), u.reg_read(UC_ARM64_REG_PC))


# Every main has these windows: LDXR w17, [x16] / op / STXR w18 / CBZ w18 to a block outside the window.
# 1.2.1 compiled the function around 1.1.2's 0x74cb68 without x18, so it has only one of them.
WINDOWS = {'v0': [0x72AFDC, 0x7AA630], 'v112': [0x74CB68, 0x80B3A0], 'v121': [0x7FF694]}


def x18_exit_windows(image, nso):
    """All exclusive windows whose STXR status register is w18 and whose CBZ on it leaves the window."""
    start, size = struct.unpack_from('<II', nso.read_bytes()[:0x100], 0x14)
    words = memoryview(image)[start:start + size].cast('I')
    found = []
    for i in range(len(words) - 3):
        a, c, d = words[i], words[i + 2], words[i + 3]
        if a & 0xbfff7c00 != 0x885f7c00 or c & 0xbfe07c00 != 0x88007c00 or (c >> 16) & 31 != 18: continue
        if d & 0x7e000000 != 0x34000000 or d & 31 != 18: continue
        at = start + 4 * i
        if not at <= nsoimg.direct_target(d, at + 12) < at + 16: found.append(at)
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('directory', type=Path)
    ap.add_argument('nso', type=Path)
    ap.add_argument('--version', choices=sorted(WINDOWS), default='v0')
    args = ap.parse_args()
    original = nsoimg.load(args.nso)
    patched = (args.directory / 'main.patched.bin').read_bytes()
    header = nsoimg.header(args.directory, 'main')
    starts = WINDOWS[args.version]
    scanned = x18_exit_windows(original, args.nso)
    assert scanned == starts, f'w18 external-exit windows in this main: {[hex(x) for x in scanned]}'
    cases = 0
    for start in starts:
        for failure in [False, True]:
            for seed in range(64):
                expected = run(original, patched, header, start, failure, seed, False)
                actual = run(original, patched, header, start, failure, seed, True)
                assert expected == actual, (hex(start), failure, seed)
                cases += 1
    negative_controls = 0
    # Reproduce the wrong fallthrough edge in the actual generated trampoline.
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.detail = True
    for start in starts:
        trampoline = target(struct.unpack_from('<I', patched, start)[0], start)
        fallthrough = next(i for i in md.disasm(patched[trampoline:trampoline + 128], trampoline)
                           if i.mnemonic == 'b' and i.operands[0].imm == start + 16)
        taken = next(md.disasm(original[start + 12:start + 16], start + 12)).operands[-1].imm
        broken = bytearray(patched)
        delta = taken - fallthrough.address
        struct.pack_into('<I', broken, fallthrough.address, 0x14000000 | ((delta >> 2) & 0x3FFFFFF))
        try:
            run(original, broken, header, start, True, 15, True)
        except AssertionError:
            negative_controls += 1
        else:
            raise AssertionError('Wrong fallthrough negative control was not detected')
    print(json.dumps({'status': 'pass', 'cases': cases, 'windows': [hex(x) for x in starts],
                      'wrong_exit_negative_controls_detected': negative_controls,
                      'success': 'real LDXR/STXR without deexcl',
                      'failure': 'injected failed-store status and restored memory',
                      'scope': 'register/stack/branch semantics; not a concurrency proof'}))


if __name__ == '__main__':
    main()
