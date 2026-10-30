#!/usr/bin/env python3
"""Replay the two registered threading cuts against the user's original NSOs.

Usage: python3 dm/tools/check_thread_cuts.py <FFT main NSO> <Link's Awakening main NSO>
Requires lz4 and unicorn. No game bytes are stored here or written to disk.
The default checks the negative controls and the registered fixes; --original
asserts the original programs are safe and deliberately fails on both bugs.

This is a controlled instruction replay, not a guest scheduler or a gameplay
test. FFT's SDK event calls are replaced with a FIFO auto-clear event; Link's
Awakening is paused between completion publication and its later release.
"""
import argparse
import importlib.util
from pathlib import Path
import struct

from unicorn import Uc, UcError, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X22, UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_PC

from nso2elf import load

BASE, STATE, STACK, STOP = 0x10000000, 0x40000000, 0x50000000, 0x60000000
GAMES = Path(__file__).resolve().parents[1] / 'games'


class Replay:
    def __init__(self, path, game, patched):
        img, _, bss, bid = load(path)
        spec = importlib.util.spec_from_file_location('thread_cuts', GAMES / game / 'patches.py')
        cuts = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cuts)
        build = bid[:16].hex()
        if build not in cuts.CUTS:
            raise ValueError(f'{path}: unregistered {game} main build {build}')
        self.vm = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.vm.mem_map(BASE, (len(img) + bss + 0xfff) & ~0xfff)
        self.vm.mem_write(BASE, img)
        for off, before, after, _ in cuts.CUTS[build]:
            old = bytes.fromhex(before)
            if img[off:off + len(old)] != old:
                raise ValueError(f'{path}: original instructions differ at {off:#x}')
            if patched:
                self.vm.mem_write(BASE + off, bytes.fromhex(after))
        # Relocate the original vtables, including the completion destructor.
        mod = struct.unpack_from('<I', img, 4)[0]
        dyn = mod + struct.unpack_from('<i', img, mod + 4)[0]
        tags = {}
        while True:
            tag, value = struct.unpack_from('<qQ', img, dyn)
            dyn += 16
            if not tag:
                break
            tags[tag] = value
        for off in range(tags[7], tags[7] + tags[8], 24):
            addr, info, addend = struct.unpack_from('<QQq', img, off)
            if info & 0xffffffff == 1027:  # R_AARCH64_RELATIVE
                self.put(BASE + addr, BASE + addend)
        for addr in (STATE, STACK, STOP):
            self.vm.mem_map(addr, 0x10000)

    def put(self, addr, value):
        self.vm.mem_write(addr, struct.pack('<Q', value))

    def get(self, addr):
        return struct.unpack('<Q', self.vm.mem_read(addr, 8))[0]

    def call(self, off, arg):
        self.vm.reg_write(UC_ARM64_REG_X0, arg)
        self.vm.reg_write(UC_ARM64_REG_LR, STOP)
        self.vm.reg_write(UC_ARM64_REG_SP, STACK + 0xfff0)
        self.vm.emu_start(BASE + off, STOP, count=1000)
        if self.vm.reg_read(UC_ARM64_REG_PC) != STOP:
            raise AssertionError(f'instruction budget exhausted at {off:#x}')


def fft(path, patched):
    r = Replay(path, '010038B015560000', patched)
    vm = r.vm
    wrapper, event = STATE, STATE + 0x100
    r.put(wrapper, event)
    waiters, ready = [], []
    signals = 0

    def sdk(uc, pc, size, data):
        nonlocal signals
        if uc.reg_read(UC_ARM64_REG_X0) != event:
            raise AssertionError('event wrapper passed the wrong event pointer')
        if pc == BASE + 0x651630:
            signals += 1
            if waiters:
                ready.append(waiters.pop(0))
        uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

    for off in (0x651630, 0x651640):
        vm.hook_add(UC_HOOK_CODE, sdk, begin=BASE + off, end=BASE + off)
    results = []
    for order in ([0], [0, 1], [1, 0]):
        for target in order:
            waiters[:] = order
            ready.clear()
            signals = 0
            r.put(wrapper + 8, 0)
            r.call(0x12554, wrapper)
            done = False
            while ready:
                worker = ready.pop(0)
                r.call(0x12500, wrapper)
                if worker == target:
                    done = True
                else:
                    waiters.append(worker)
                if signals > 4:
                    raise AssertionError('wake relay did not quiesce')
            print(f'FFT {"patched" if patched else "original"}: waiters={order}, job={target}, done={done}, signals={signals}')
            results.append(done)
    return all(results)


def awakening(path, patched, late):
    r = Replay(path, '01006BB00C6F0000', patched)
    vm = r.vm
    world, manager, manager_vtable = STATE, STATE + 0x1000, STATE + 0x2000
    task = world + 0xc8
    r.put(manager, manager_vtable)
    r.put(manager_vtable + 0xa0, STOP)

    def rebuild():
        # Actual in-place construction and the subsequent mTm assignment in
        # the first world's step. The scene's getter supplies our manager.
        vm.reg_write(UC_ARM64_REG_X22, world)
        vm.emu_start(BASE + 0x157a4, BASE + 0x157c0, count=20)
        vm.reg_write(UC_ARM64_REG_X0, manager)
        vm.emu_start(BASE + 0x157dc, BASE + 0x157e0, count=1)
        assert r.get(task) == BASE + 0x1c51308 and r.get(task + 0x10) == manager

    rebuild()
    if late:
        # The preceding worker has unlocked; main has constructed generation
        # two, but generation one's release has yet to run on another thread.
        rebuild()
    r.call(0x10cd070, task)
    if not late:
        rebuild()
    try:
        r.call(0x10ccf20, task)  # the following simulation removes a reference
        ok = vm.reg_read(UC_ARM64_REG_X0) == manager and vm.reg_read(UC_ARM64_REG_X1) == task
    except UcError:
        if vm.reg_read(UC_ARM64_REG_PC) != BASE + 0x10ccf28 or r.get(task + 0x10) != 0:
            raise
        ok = False
    print(f'Awakening {"patched" if patched else "original"}: late={late}, mTm={r.get(task + 0x10):#x}, safe={ok}')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('fft_main', type=Path)
    ap.add_argument('awakening_main', type=Path)
    ap.add_argument('--original', action='store_true')
    args = ap.parse_args()
    original_fft = fft(args.fft_main, False)
    # A normal lifetime passes without the cut. Only the late release fails.
    normal = awakening(args.awakening_main, False, False)
    original_awakening = awakening(args.awakening_main, False, True)
    if args.original:
        return 0 if original_fft and normal and original_awakening else 1
    fixed_fft = fft(args.fft_main, True)
    fixed_normal = awakening(args.awakening_main, True, False)
    fixed_late = awakening(args.awakening_main, True, True)
    ok = not original_fft and normal and not original_awakening and fixed_fft and fixed_normal and fixed_late
    print('PASS: both negative controls fail and both registered fixes pass' if ok else 'FAIL: threading cut regression')
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
