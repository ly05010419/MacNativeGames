#!/usr/bin/env python3
"""Reproduce a predicated block-tail skipping an unpredicated pending write.

Generate tiny original SASS fixtures, translate them through the production
service, and reuse probe-gpu-f2ip's scalar production-backend dispatch harness.
The CPU oracle is the integer population count, independent of translation.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def inst(op, rd=0, ra=0, b=0, c=0, pred=7, write=7, wait=0):
    control = (0x000fe200 & ~(7 << 14)) | (write << 14) | (wait << 20)
    return struct.pack('<IIII', op | pred << 12 | rd << 16 | ra << 24, b, c, control)


def mov(rd, value):
    return inst(0x802, rd, b=value, c=0xf00)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    ap.add_argument('--spirv-cross', type=Path, required=True)
    a = ap.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        raise ValueError('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen([str(a.service), '--service', '--spirv-cross', str(a.spirv_cross), '--out', str(work), '--chain', 'oot-scoreboard-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cases = []
    for barrier in range(6):
        for flag in (0, 1):
            for value in (0, 0x80000001, 0x5a5aa5a5, 0xffffffff):
                key = f'scoreboard-{len(cases):03d}'
                old = 0xc2300000
                code = inst(0xa02, 0, b=0x310 << 6, c=0xf00) + inst(0xa02, 1, b=0x314 << 6, c=0xf00)
                code += mov(25, flag) + mov(14, old) + mov(18, value)
                # LOP3 (R25 & 1) -> P0; form and field positions from actual
                # task code 0x2540, not from a generated MSL expectation.
                code += inst(0x812, 255, 25, b=1, c=0x0780c0ff)
                # An earlier branch makes the instruction after the predicated
                # store a real block entry, as in the failing original shader.
                code += inst(0x947, b=(2 * 16 // 4) << 2, c=0x03800000, pred=0)
                code += inst(0x309, 14, b=18, write=barrier)
                code += inst(0x386, ra=0, b=14, c=0x900, pred=0)
                code += inst(0x386, ra=0, b=14, c=0x900, wait=1 << barrier)
                code += inst(0x94d, c=0x03800000)
                path = work / (key + '.code')
                path.write_bytes(code)
                request = dict(variant=key, stage='cs', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False)
                process.stdin.write(json.dumps(request) + '\n')
                process.stdin.flush()
                reply = json.loads(process.stdout.readline())
                if not reply['ok']:
                    raise RuntimeError(reply)
                cases.append(dict(source=str(work / (key + '_b.metal')), reflection=str(work / (key + '_b.json')), expected=old if flag else value.bit_count(), label=f'barrier={barrier} guard={flag} input={value:#x}'))
    process.stdin.close()
    if process.wait():
        raise RuntimeError('shader service failed')
    manifest = work / 'cases.json'
    manifest.write_text(json.dumps(cases, indent=2) + '\n')
    print(manifest)


if __name__ == '__main__':
    main()
