#!/usr/bin/env python3
"""Check formatted SUST integer images and their NVN image-table slots.

Generate independent SASS fixtures; probe-gpu-sured reads all 16 output pixels
through the production backend. Only pixel (2, 3) may change, to the source word.
"""
import argparse
import json
from pathlib import Path
import subprocess
from f2ip_gpu import instruction, mov


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
    p = subprocess.Popen([str(a.service), '--service', '--spirv-cross', str(a.spirv_cross), '--out', str(work), '--chain', 'oot-formatted-image-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cases = []
    for slot in (0, 1, 2, 8):
        for fmt in ('R32Uint', 'R32Sint', 'R16G16Uint'):
            signed = fmt == 'R32Sint'
            rg16 = fmt == 'R16G16Uint'
            for word in (0, 0x3f800000, 0x80000001, 0xffffffff):
                key = f'formatted-image-{len(cases):03d}'
                handle = (0x120 + slot * 8) // 4
                code = mov(4, 2) + mov(5, 3) + mov(0, word & 65535 if rg16 else word)
                if rg16:
                    code += mov(1, word >> 16)
                # SUST.P.2D.R [R4], R0, c[2][image-table + slot*8].
                # Opcode/fields match the original postprocess SUST instruction.
                code += instruction(0xb9b, ra=4, b=(2 << 22) | (handle << 8) | (3 << 29), c=0x00100000 | ((3 if rg16 else 1) << 8))
                code += instruction(0x94d, c=0x03800000)
                path = work / (key + '.code')
                path.write_bytes(code)
                request = dict(variant=key, stage='cs', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False, img={str(slot): dict(type='2d', fmt=fmt)})
                p.stdin.write(json.dumps(request) + '\n')
                p.stdin.flush()
                reply = json.loads(p.stdout.readline())
                if not reply['ok']:
                    raise RuntimeError(reply)
                cases.append(dict(source=str(work / (key + '_b.metal')), reflection=str(work / (key + '_b.json')), initial=0x12345678, expected=word, target=14, signed=signed, slot=slot, format='rg16ui' if rg16 else 'r32', label=f'formatted slot={slot} format={fmt} word={word:#x}'))
    p.stdin.close()
    if p.wait():
        raise RuntimeError('shader service failed')
    manifest = work / 'cases.json'
    manifest.write_text(json.dumps(cases, indent=2) + '\n')
    print(manifest)


if __name__ == '__main__':
    main()
