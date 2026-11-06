#!/usr/bin/env python3
"""Translate actual ISBEWR/ISBERD encodings; compare payload reads with an independent GPU oracle.

The production service emits the task body unchanged. A compute wrapper exposes its
shared stage memory to readback, so the test does not depend on mesh rasterization.
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
from ampere_scoreboard_gpu import inst, mov


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--service', type=Path, required=True)
    a = p.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        p.error('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)
    ctl = bytearray(0xe0)
    struct.pack_into('<HH', ctl, 0x58, 1, 128)
    service = subprocess.Popen([str(a.service), '--service', '--spirv-cross', '/usr/bin/false',
                                '--out', str(work), '--chain', 'oot-stage-payload-regression'],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    cases, sources = [], ['#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n']
    for mode in (0x38, 0x78, 0xb8):
        for base, offset in ((0, 0), (4, 16), (32, 36)) if mode != 0x78 else ((0, 0),):
            for value in (0, 0x80000001, 0x5a5aa5a5, 0xffffffff):
                key = f'payload-{len(cases)}'
                # Original OOT task 5d3a0960: ISBEWR [Ra+offset],R2 followed by
                # ISBERD from the same output word. Bit 79 selects output ISBE.
                code = bytes(128) + mov(1, base) + mov(2, value)
                code += inst(0x927, ra=1, b=offset << 8 | 2, c=0xb800)
                code += inst(0x923, rd=3, ra=1, b=offset << 8, c=mode << 8)
                code += inst(0x927, ra=255, b=0x60 << 8 | 3, c=0xb800)
                code += inst(0x94d, c=0x03800000)
                path = work / (key + '.code')
                path.write_bytes(code)
                request = dict(variant=key, stage='ts', arch='sm86', code=str(path), mesh_ctl=ctl.hex(),
                               c1='', local=[1, 1, 1], depth_minus_w=True)
                service.stdin.write(json.dumps(request) + '\n')
                service.stdin.flush()
                reply = json.loads(service.stdout.readline())
                if not reply.get('ok'):
                    raise RuntimeError(reply)
                source = (work / (key + '_b.metal')).read_text().split('struct dm_payload_t')[0]
                signature = re.search(r'void dm_stage_body\(threadgroup spvUnsafeArray<uint, (\d+)>& \w+\)', source)
                if not signature:
                    raise RuntimeError('unexpected task body arguments')
                words = int(signature.group(1))
                source = re.sub(r'^#include .*$', '', source, flags=re.M)
                number = len(cases)
                sources.append(f'namespace fixture_{number} {{\n{source}\n}}\n')
                sources.append(f'''kernel void probe_{number}(device uint* out [[buffer(0)]]) {{
    threadgroup fixture_{number}::spvUnsafeArray<uint, {words}> memory;
    for (uint i=0;i<{words};i++) memory[i]=0x11223344u;
    memory[0]=37u;
    fixture_{number}::dm_stage_body(memory);
    out[0]=memory[60];
}}\n''')
                expected = value if mode == 0xb8 else 37 if mode == 0x78 else 0x11223344
                cases.append(dict(function=f'probe_{number}', expected=expected, mode=mode, base=base, offset=offset))
    service.stdin.close()
    if service.wait():
        raise RuntimeError('shader service failed')
    (work / 'probe.metal').write_text(''.join(sources))
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    print(f'{len(cases)} ISBERD GPU cases prepared in {work}')


if __name__ == '__main__':
    main()
