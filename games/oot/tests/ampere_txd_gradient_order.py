#!/usr/bin/env python3
"""Ampere TXD passes its derivatives interleaved: ddx.u, ddy.u, ddx.v, ddy.v (patch 0032).

The translator's texture operation takes dPdx, then dPdy. Passing the registers through unchanged gave Metal (ddx.u, ddy.u) and
(ddx.v, ddy.v), which stretched Ocarina of Time's terrain albedo into long streaks (fragment shader 2a712d1a62f11442, 2026-11-09).
The fixture loads four distinct constants into the derivative registers; the expected MSL names them in the swapped order.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess

from ampere_scoreboard_gpu import inst, mov

FLOATS = (1.0, 2.0, 4.0, 8.0)


def txd(rd, ra, rb, handle=0x40):
    bank, dim_2d, mask, no_residency = 2, 1, 1, 7
    b = rb | handle << 8 | bank << 22 | dim_2d << 29
    c = mask << 8 | no_residency << 17
    return inst(0xb6c, rd, ra, b=b, c=c)


def f32(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


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
    code = inst(0xa02, 0, b=0x310 << 6, c=0xf00) + inst(0xa02, 1, b=0x314 << 6, c=0xf00)
    code += mov(20, f32(.25)) + mov(21, f32(.75))
    for i, value in enumerate(FLOATS):
        code += mov(24 + i, f32(value))
    code += txd(14, 20, 24) + inst(0x386, ra=0, b=14, c=0x900) + inst(0x94d, c=0x03800000)
    path = work / 'txd.code'
    path.write_bytes(code)
    service = subprocess.Popen([str(a.service), '--service', '--spirv-cross', '/usr/bin/true', '--out', str(work), '--chain', 'oot-txd-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    service.stdin.write(json.dumps(dict(variant='txd', stage='cs', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False)) + '\n')
    service.stdin.flush()
    reply = json.loads(service.stdout.readline())
    service.stdin.close()
    if service.wait() or not reply['ok']:
        raise RuntimeError(reply)
    msl = (work / 'txd_b.metal').read_text()
    found = re.search(r'gradient2d\(([^;]*?)\)\)', msl)
    if not found:
        raise RuntimeError('no gradient2d in the MSL:\n' + msl)
    numbers = [float(x) for x in re.findall(r'\d+\.\d+', found.group(1))]
    expected = [FLOATS[0], FLOATS[2], FLOATS[1], FLOATS[3]]
    print('gradient', found.group(0), '->', numbers)
    if numbers != expected:
        raise SystemExit(f'FAILED: derivatives {numbers}, expected {expected} (dPdx = ddx.u, ddx.v; dPdy = ddy.u, ddy.v)')
    print('PASSED')


if __name__ == '__main__':
    main()
