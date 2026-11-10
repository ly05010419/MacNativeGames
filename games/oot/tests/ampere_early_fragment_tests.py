#!/usr/bin/env python3
"""A Switch 2 pixel shader translated with its early fragment tests flag declares [[early_fragment_tests]].

The runtime reads the flag from the NVN2 control section, byte +0x5b (totk_shader_rt.m variant_request; before 2026-11-10
only NVN1's +0x718 was read and no Ampere pixel shader got the attribute), and leaves a shader that writes depth alone.
This checks the translator's half: the request's "early_fragment_tests" reaches the MSL entry point, and its absence
leaves the entry point plain.
Usage: ampere_early_fragment_tests.py --service <totk_xlate> --work <folder outside the checkout>
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from ampere_depth_register import f32, header
from ampere_scoreboard_gpu import inst, mov


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
    code = header(0xf, 0) + struct.pack('<IIII', 0, 0, 0, 0) * 3
    for r in range(4):
        code += mov(r, f32(1.0))
    code += inst(0x94d, c=0x03800000)
    path = work / 'early.code'
    path.write_bytes(code)
    service = subprocess.Popen([str(a.service), '--service', '--spirv-cross', '/usr/bin/true', '--out', str(work), '--chain', 'oot-early-tests-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    found = {}
    for name, flag in (('early', True), ('plain', False)):
        rq = dict(variant=name, stage='ps', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False)
        if flag:
            rq['early_fragment_tests'] = True
        service.stdin.write(json.dumps(rq) + '\n')
        service.stdin.flush()
        reply = json.loads(service.stdout.readline())
        if not reply['ok']:
            raise RuntimeError(reply)
        found[name] = 'early_fragment_tests' in (work / (name + '_b.metal')).read_text()
    service.stdin.close()
    service.wait()
    print('with the flag:', found['early'], '| without:', found['plain'])
    if found != {'early': True, 'plain': False}:
        raise SystemExit('FAILED: the attribute must follow the request flag')
    print('PASSED')


if __name__ == '__main__':
    main()
