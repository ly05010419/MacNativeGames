#!/usr/bin/env python3
"""Check the hair / cloth clock veneers of 1.0.0, 1.1.2 and 1.2.1 (runtime/game_patches.c patch_cloth_clock) by running
them in Unicorn on the image the production C patches produce (scripts/v143_timing_fixture.c, built against
game_patches.c for each version).

Per inline place: the fmul veneer leaves measured seconds * factor in the result register, the ratio-store veneer
writes measured 30 Hz ratio * factor into the clock's ratio slot (sp- or x29-relative), and both restore x9, q31, sp
and x29. The call veneer scales s2 / s3 by the measured ratio and enters the shared update with the caller's return
address. The unpatched image must give factor / 30 (4x at 120 FPS). Needs the work tree's rewritten/<version> images
and the unicorn module.

usage: verify_cloth_clock.py [v0 v112 v121]
"""
import os
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_Q31, UC_ARM64_REG_SP,
                                 UC_ARM64_REG_X9, UC_ARM64_REG_X29)

ROOT = Path(__file__).resolve().parents[1]
# version: (define, text_end, inline places (fmul, ratio store, r, f, slot offset, x29 pair), call, shared update)
VERSIONS = {
    'v0': (None, 0x2aa6a10, [(0x7b5988, 0x7b598c, 0, 8, -0x40, True), (0x7b6b28, 0x7b6b2c, 0, 8, -0x40, True),
                             (0x7f5ec4, 0x7f5ed0, 1, 0, 0x114, False)], 0x7b6358, 0x82c224),
    'v112': ('TOTK_GAME_V112', 0x2b17b30, [(0x9d1e64, 0x9d1e74, 0, 8, 0x104, False), (0x9d2d34, 0x9d2d2c, 0, 8, 0x104, False),
                                           (0x9e7648, 0x9e764c, 1, 0, 0x134, False)], 0x9d2774, 0x88a998),
    'v121': ('TOTK_GAME_V121', 0x2b19a50, [(0x82192c, 0x821928, 0, 8, 0x124, False), (0x822a64, 0x822a40, 0, 8, 0x124, False),
                                           (0x83182c, 0x831830, 1, 0, 0x134, False)], 0x8222f0, 0x89822c),
}
STACK, FRAME = 0x70000000, 0x70008000
SENTINEL_X9, SENTINEL_Q31 = 0x1122334455667788, 0x0123456789abcdeffedcba9876543210


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def bits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def reg_s(n):
    from unicorn import arm64_const
    return getattr(arm64_const, f'UC_ARM64_REG_S{n}')


def patched_image(version):
    define, text_end, _, _, _ = VERSIONS[version]
    source = ROOT / f'rewritten/{version}/main.patched.bin'
    fixture, target = ROOT / f'build/cloth-fixture-{version}', ROOT / f'build/cloth-fixture-{version}.bin'
    cmd = ['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Iruntime', '-Ithird_party/d2r_gfx/gfx/nvn/include',
           '-Ibuild', 'scripts/v143_timing_fixture.c', 'runtime/game_patches.c', '-o', str(fixture)]
    if define:
        cmd.insert(3, f'-D{define}=1')
    subprocess.run(cmd, check=True)
    env = os.environ | {'TOTK_REALTIME': '1', 'TOTK_FPS60': '1', 'TOTK_CLOTH_REALTIME': '1', 'TOTK_OUTPUT_RES': '0'}
    run = subprocess.run([str(fixture), str(source), str(target), hex(text_end)], capture_output=True, text=True, env=env)
    assert run.returncode == 0, run.stderr
    assert 'cloth/hair clock: real-time seconds and ratio' in run.stderr, run.stderr
    return source.read_bytes(), target.read_bytes()


def machine(image, data_off, hz):
    mu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    mu.mem_map(0, max((len(image) + 0xfff) & ~0xfff, data_off + 0x4000))   # the raw image has no real-time page
    mu.mem_write(0, image)
    mu.mem_write(data_off, struct.pack('<fff', 30 / hz, 1 / hz, 60 / hz))
    mu.mem_map(STACK, 0x10000)
    mu.reg_write(UC_ARM64_REG_SP, STACK + 0x4000)
    mu.reg_write(UC_ARM64_REG_X29, FRAME)
    mu.reg_write(UC_ARM64_REG_X9, SENTINEL_X9)
    mu.reg_write(UC_ARM64_REG_Q31, SENTINEL_Q31)
    return mu


def get_s(mu, n):
    return struct.unpack('<f', struct.pack('<I', mu.reg_read(reg_s(n)) & 0xffffffff))[0]


def preserved(mu, what):
    assert mu.reg_read(UC_ARM64_REG_SP) == STACK + 0x4000, f'{what}: sp changed'
    assert mu.reg_read(UC_ARM64_REG_X29) == FRAME, f'{what}: x29 changed'
    assert mu.reg_read(UC_ARM64_REG_X9) == SENTINEL_X9, f'{what}: x9 changed'
    assert mu.reg_read(UC_ARM64_REG_Q31) == SENTINEL_Q31, f'{what}: q31 changed'


def check(version):
    _, text_end, places, call, update = VERSIONS[version]
    raw, patched = patched_image(version)
    data_off = (len(raw) + 0x3fff) & ~0x3fff
    cases = 0
    for hz in (20, 30, 60, 90, 120):
        for factor in (0.0, 0.25, 1.0, 2.0):
            for fmul, store, r, f, off, pair in places:
                # fmul: s<r> held 1/30
                mu = machine(patched, data_off, hz)
                mu.reg_write(reg_s(f), bits(factor)); mu.reg_write(reg_s(r), bits(f32(1 / 30)))
                mu.emu_start(fmul, fmul + 4, count=20)
                assert mu.reg_read(UC_ARM64_REG_PC) == fmul + 4, f'{version} {fmul:#x}: did not return'
                assert abs(get_s(mu, r) - factor / hz) < 1e-6, (version, hex(fmul), hz, factor, get_s(mu, r))
                assert abs(get_s(mu, f) - factor) < 1e-6, f'{version} {fmul:#x}: factor changed'
                preserved(mu, f'{version} {fmul:#x}')
                # ratio store
                mu = machine(patched, data_off, hz)
                seconds = f32(factor / hz)
                mu.reg_write(reg_s(f), bits(factor)); mu.reg_write(reg_s(r), bits(seconds))
                mu.emu_start(store, store + 4, count=20)
                assert mu.reg_read(UC_ARM64_REG_PC) == store + 4, f'{version} {store:#x}: did not return'
                slot = FRAME + off if pair else STACK + 0x4000 + off
                got = struct.unpack('<f', mu.mem_read(slot + (4 if pair else 0), 4))[0]
                assert abs(got - factor * 30 / hz) < 1e-5, (version, hex(store), hz, factor, got)
                if pair:
                    assert abs(struct.unpack('<f', mu.mem_read(slot, 4))[0] - seconds) < 1e-7, f'{version} {store:#x}: seconds slot'
                assert abs(get_s(mu, f) - factor) < 1e-6, f'{version} {store:#x}: factor changed'
                preserved(mu, f'{version} {store:#x}')
                cases += 2
            # the call into the shared update: s2 = factor / 30, s3 = factor
            mu = machine(patched, data_off, hz)
            mu.reg_write(reg_s(2), bits(f32(factor / 30))); mu.reg_write(reg_s(3), bits(factor))
            mu.emu_start(call, update, count=20)
            assert mu.reg_read(UC_ARM64_REG_PC) == update, f'{version} call did not reach the update'
            assert mu.reg_read(UC_ARM64_REG_LR) == call + 4, f'{version} call: return address changed'
            assert abs(get_s(mu, 2) - factor / hz) < 1e-6 and abs(get_s(mu, 3) - factor * 30 / hz) < 1e-5, (version, hz, factor)
            preserved(mu, f'{version} call')
            cases += 1
    # negative control: the shipping code advances factor / 30 per frame (4x at 120 FPS)
    fmul, _, r, f, _, _ = places[0]
    mu = machine(raw, data_off, 120)
    mu.reg_write(reg_s(f), bits(1.0)); mu.reg_write(reg_s(r), bits(f32(1 / 30)))
    mu.emu_start(fmul, fmul + 4, count=1)
    assert abs(get_s(mu, r) * 120 - 4) < 1e-4, 'negative control must reproduce the 4x clock at 120 FPS'
    # only the four kinds of site words changed near each place
    for fmul, store, *_ in places:
        for o in range(min(fmul, store) - 32, max(fmul, store) + 36, 4):
            if o not in (fmul, store):
                assert raw[o:o + 4] == patched[o:o + 4], f'{version}: unexpected change at {o:#x}'
    print(f'{version}: pass, {cases} cases')


def main():
    os.chdir(ROOT)
    for version in sys.argv[1:] or VERSIONS:
        check(version)


if __name__ == '__main__':
    main()
