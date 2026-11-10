#!/usr/bin/env python3
"""GPU canaries around the stage allocation, using original STAGEOUT encodings.

Translate complete-block exports at partial primitive bounds. The old-size
control must overwrite its canary; the production allocation must preserve it.
Generated code and binaries stay in the supplied external work directory.
Run cases with stage_payload_gpu.m (Metal 4).
"""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
from ampere_scoreboard_gpu import inst, mov


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    ap.add_argument('--spirv-cross', type=Path, required=True)
    a = ap.parse_args()
    work = a.work.resolve()
    root = Path(__file__).resolve().parents[3]
    if work == root or root in work.parents:
        ap.error('generated fixtures must remain outside the checkout')
    work.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen([str(a.service), '--service', '--spirv-cross', str(a.spirv_cross),
                             '--out', str(work), '--chain', 'oot-mesh-index-bounds'],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    sources = ['#include <metal_stdlib>\n#include <simd/simd.h>\nusing namespace metal;\n']
    cases = []
    for max_p in (1, 31, 32, 33, 63, 64, 65, 95, 96, 97, 126, 127, 128, 255, 256, 511, 512):
        ctl = bytearray(0xe0)
        struct.pack_into('<HH', ctl, 0x58, 1, 0)
        ctl[0x5f] = 3
        struct.pack_into('<HH', ctl, 0x60, 64, max_p)
        header = bytearray(128)
        header[0x35] = 0xf0
        # Export the last index of every lane in the final 32-primitive block.
        # The expected address follows the guest's block contract, independent
        # of the translator's reported threadgroup allocation.
        last = 4 + 3 * ((max_p + 31) // 32 * 32) - 1
        code = bytes(header) + mov(1, last) + mov(2, 0x5a)
        code += inst(0x927, ra=1, b=2, c=0x8000) + inst(0x94d, c=0x03800000)
        key = f'index-bound-{max_p}'
        path = work / (key + '.code')
        path.write_bytes(code)
        rq = dict(variant=key, stage='ms', arch='sm86', code=str(path), mesh_ctl=ctl.hex(),
                  c1='', local=[1, 1, 1], depth_minus_w=True, stage_device=False)   # the compute wrapper below reads threadgroup stage memory
        proc.stdin.write(json.dumps(rq) + '\n')
        proc.stdin.flush()
        reply = json.loads(proc.stdout.readline())
        if not reply.get('ok'):
            raise RuntimeError(reply)
        source = (work / (key + '_b.metal')).read_text().split('struct dm_payload_t')[0]
        match = re.search(r'dm_stage_body\(threadgroup spvUnsafeArray<uint, (\d+)>& (\w+)\)', source)
        if not match:
            raise RuntimeError('unexpected body arguments')
        words = int(match[1])
        primitive_word = int(re.search(r'uint np=min\(\w+\[(\d+)\]',
                            (work / (key + '_b.metal')).read_text())[1])
        for old in (False, True):
            # The production emitted body is unchanged. Extra backing words
            # expose out-of-allocation stores without invoking undefined GPU
            # out-of-bounds behaviour in the test itself.
            nominal = primitive_word + ((4 + 3 * max_p + 3) // 4 if old else words - primitive_word)
            backing = words + 8
            text = source.replace(f', {words}>', f', {backing}>')
            text = re.sub(r'^#include .*$', '', text, flags=re.M)
            n = len(cases)
            sources.append(f'namespace fixture_{n} {{\n{text}\n}}\n')
            sources.append(f'''kernel void probe_{n}(device uint* out [[buffer(0)]]) {{
    threadgroup fixture_{n}::spvUnsafeArray<uint, {backing}> memory;
    for(uint i=0;i<{backing}u;i++)memory[i]=i<{nominal}u?0u:0xc5c5c5c5u;
    fixture_{n}::dm_stage_body(memory);
    uint damaged=0u;
    for(uint i={nominal}u;i<{backing}u;i++)damaged|=memory[i]!=0xc5c5c5c5u;
    out[0]=damaged;
}}\n''')
            cases.append(dict(function=f'probe_{n}', expected=int(old and primitive_word + last // 4 >= nominal),
                              max_p=max_p, old_control=old, nominal_words=nominal,
                              required_last_byte=primitive_word * 4 + last))
    proc.stdin.close()
    if proc.wait():
        raise RuntimeError('service failed')
    (work / 'probe.metal').write_text(''.join(sources))
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    assert any(c['old_control'] and c['max_p'] == 126 and c['expected'] == 1 for c in cases)
    print(f'{len(cases)} GPU allocation-canary cases, including the old maxP126 overflow')


if __name__ == '__main__':
    main()
