#!/usr/bin/env python3
"""Two predicated deferred writes of one register: only the executed one may land.

Patch 0031. NVN2 compilers issue complementary predicated variable-latency writes of
one register without a wait between them (OOT tree mesh shader 604134a5: "@!P4 LDC
R0, c[7]" on barrier 2, then "@P4 LDC.U8 R0, c[6]" on barrier 0). The translator used
to drop the first pending write when the second was issued and to commit a skipped
write's issue-time snapshot, so R0 kept a stale value whenever P4 was false.

Each fixture: P0 = flag & 1; R14 = old; "@!P0 POPC R14, R18" (barrier A) and
"@P0 POPC R14, R19" (barrier B); the barriers are waited on in either order, then
R14 is stored. The CPU oracle is the population count of the source the executed
instruction read. Fixtures are translated through the production service and, with
--runtime and --modules, run with probe-gpu-f2ip's production-backend dispatch.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

from ampere_scoreboard_gpu import inst, mov


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    ap.add_argument('--spirv-cross', type=Path, required=True)
    ap.add_argument('--runtime', type=Path, help='oot-native; with --modules runs the GPU check')
    ap.add_argument('--modules', type=Path)
    ap.add_argument('--game-data', type=Path)
    a = ap.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        raise ValueError('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen([str(a.service), '--service', '--spirv-cross', str(a.spirv_cross), '--out', str(work), '--chain', 'oot-predicated-writes-regression'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cases = []
    old = 0xc2300000
    for first, second in ((2, 0), (0, 2), (1, 3), (3, 1)):
        for first_waited in (True, False):
            for flag in (0, 1):
                for x, y in ((0x0000000f, 0x7fffffff), (0xffffffff, 0x00000001), (0x80000001, 0x5a5aa5a5)):
                    key = f'predwrite-{len(cases):03d}'
                    code = inst(0xa02, 0, b=0x310 << 6, c=0xf00) + inst(0xa02, 1, b=0x314 << 6, c=0xf00)
                    code += mov(25, flag) + mov(14, old) + mov(18, x) + mov(19, y)
                    code += inst(0x812, 255, 25, b=1, c=0x0780c0ff)          # P0 = R25 & 1 (as ampere_scoreboard_gpu.py)
                    code += inst(0x309, 14, b=18, pred=8, write=first)      # @!P0 POPC R14, R18
                    code += inst(0x309, 14, b=19, pred=0, write=second)     # @P0  POPC R14, R19
                    order = (first, second) if first_waited else (second, first)
                    code += inst(0x802, 20, b=0, c=0xf00, wait=1 << order[0])   # MOV R20, 0 waits on one barrier
                    code += inst(0x386, ra=0, b=14, c=0x900, wait=1 << order[1])
                    code += inst(0x94d, c=0x03800000)
                    path = work / (key + '.code')
                    path.write_bytes(code)
                    request = dict(variant=key, stage='cs', arch='sm86', code=str(path), c1='', local=[1, 1, 1], depth_minus_w=False)
                    process.stdin.write(json.dumps(request) + '\n')
                    process.stdin.flush()
                    reply = json.loads(process.stdout.readline())
                    if not reply['ok']:
                        raise RuntimeError(reply)
                    expected = (y if flag else x).bit_count()
                    cases.append(dict(source=str(work / (key + '_b.metal')), reflection=str(work / (key + '_b.json')), expected=expected,
                                      label=f'barriers={first},{second} first_waited={first_waited} P0={flag} sources={x:#x},{y:#x}'))
    process.stdin.close()
    if process.wait():
        raise RuntimeError('shader service failed')
    manifest = work / 'cases.json'
    manifest.write_text(json.dumps(cases, indent=2) + '\n')
    print(manifest, len(cases), 'cases')
    if a.runtime and a.modules:
        env = dict(os.environ, OOT_F2IP_CASES=str(manifest), TOTK_SLOT_OWNER='claude', TOTK_SLOT_PURPOSE='oot-predicated-writes-gpu-regression',
                   DM_PSO_MANIFEST='off', DM_PSO_USER='/dev/null', MTL_DEBUG_LAYER='1', **({'TOTK_GAME_DATA': str(a.game_data)} if a.game_data else {}))
        subprocess.run([str(a.runtime), str(a.modules), 'probe-gpu-f2ip'], cwd=work, env=env, check=True)


if __name__ == '__main__':
    main()
