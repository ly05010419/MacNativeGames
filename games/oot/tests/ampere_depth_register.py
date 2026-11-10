#!/usr/bin/env python3
"""A pixel shader's depth register after a partly written colour target (patch 0033).

The outputs are read four registers per colour target with any component enabled, then the sample mask register, then the
depth: R(4 x enabled targets + 1). The translator counted enabled components instead, which agrees only when every enabled
target has all four. With OmapTargets 0x10f (target 0 RGBA, target 2 R) the depth is R9; R6 was read. Ocarina of Time's
half-resolution depth downsample d8bbabcb9d1fdcf1 never writes R6, so its depth came out as the constant 0.0 and every
depth-tested draw of the half-resolution transparent passes (ground mist, particles) failed (2026-11-10). The rule is also
in seedds/nxnative's docs/reference/shaders.md, "A pixel shader's depth after a partly written target".
The fixture puts 0.25 in R6 and 0.75 in R9; the MSL must store 0.75 to the depth.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

from ampere_scoreboard_gpu import inst, mov


def f32(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def header(omap_targets, omap_flags):
    """The 0x50-byte shader program header of a pixel shader: only the fields this test needs are named."""
    h = bytearray(0x50)
    struct.pack_into('<I', h, 0x00, 0x000c5482)      # common word 0: pixel shader, version 3
    struct.pack_into('<I', h, 0x14, 0x80000000)
    struct.pack_into('<I', h, 0x18, 0x0000000a)
    struct.pack_into('<I', h, 0x48, omap_targets)     # four bits per colour target
    struct.pack_into('<I', h, 0x4c, omap_flags)       # bit 0 sample mask, bit 1 depth
    return bytes(h)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    a = ap.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        raise ValueError('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)
    nop = struct.pack('<IIII', 0, 0, 0, 0)
    code = header(0x10f, 2) + nop * 3
    for r in range(5):
        code += mov(r, f32(1.0))                      # target 0 in R0-R3, target 2's red in R4
    code += mov(6, f32(.25)) + mov(9, f32(.75))
    code += inst(0x94d, c=0x03800000)
    path = work / 'depth.code'
    path.write_bytes(code)
    service = subprocess.Popen([str(a.service), '--service', '--spirv-cross', '/usr/bin/true', '--out', str(work), '--chain', 'oot-depth-register-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    service.stdin.write(json.dumps(dict(variant='depth', stage='ps', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False)) + '\n')
    service.stdin.flush()
    reply = json.loads(service.stdout.readline())
    service.stdin.close()
    if service.wait() or not reply['ok']:
        raise RuntimeError(reply)
    msl = (work / 'depth_b.metal').read_text()
    found = re.search(r'gl_FragDepth = ([^;]+);', msl)
    if not found:
        raise RuntimeError('no depth store in the MSL:\n' + msl)
    print('depth store:', found.group(0))
    if float(found.group(1)) != .75:
        raise SystemExit(f'FAILED: the depth is {found.group(1)}, expected 0.75 (R9 = 4 x 2 enabled targets + 1)')
    print('PASSED')


if __name__ == '__main__':
    main()
