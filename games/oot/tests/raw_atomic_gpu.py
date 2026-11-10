#!/usr/bin/env python3
"""Raw-pointer atomic exchange/CAS: verify returned old value and actual write.

Independent SM86 instructions use the production raw-pool GPU fixture. Pointer
and replacement come from host memory. Run with bookkeeping enabled, disabled,
then enabled again, including addresses in gaps and beyond the old pool limit.
"""
import argparse
import json
from pathlib import Path
import subprocess

from raw_pool_gpu import inst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--work', type=Path, required=True)
    ap.add_argument('--service', type=Path, required=True)
    ap.add_argument('--spirv-cross', type=Path, required=True)
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    cases = []
    for operation in ['exchange', 'compare-exchange']:
        code = inst(0xa02, 0, b=0x310 << 6, c=0xf00)
        code += inst(0xa02, 1, b=0x314 << 6, c=0xf00)
        code += inst(0x381, 2, 0, b=8 << 8, c=0xb00)   # pointer R2:R3
        code += inst(0x381, 4, 0, b=16 << 8, c=0x900)  # replacement R4
        if operation == 'compare-exchange':
            code += inst(0x381, 5, 2, c=0x900)  # original word as comparison
            code += inst(0x3a9, 6, 2, b=5, c=4 | 1 << 8 | 7 << 17)
        else:
            code += inst(0x3a8, 6, 2, b=4, c=1 << 8 | 8 << 23)
        code += inst(0x386, ra=0, b=6, c=0x900)  # return atomic's old word
        code += inst(0x94d, c=0x03800000)
        variant = 'raw-atomic-' + operation
        path = args.work / (variant + '.code')
        path.write_bytes(code)
        request = dict(variant=variant, stage='cs', arch='sm86', code=str(path),
                       c1='', local=[1, 1, 1], depth_minus_w=False)
        process = subprocess.run(
            [str(args.service), '--service', '--spirv-cross', str(args.spirv_cross),
             '--out', str(args.work), '--chain', 'oot-raw-atomic-regression'],
            input=json.dumps(request) + '\n', text=True,
            stdout=subprocess.PIPE, check=True)
        reply = json.loads(process.stdout.splitlines()[0])
        if not reply.get('ok'):
            raise RuntimeError(reply)
        for log in [1, 0, 1]:
            for index in [0, 4094, 4095, 8192]:
                for offset in [0, 4092, 4096, 16380]:
                    unmapped = offset >= 4096
                    cases.append(dict(
                        source=str(args.work / (variant + '_b.metal')),
                        reflection=str(args.work / (variant + '_b.json')),
                        raw_pool_count=8193, raw_pool_index=index,
                        raw_offset=offset, raw_store=True, raw_unmapped=unmapped,
                        raw_index=1, raw_log=log,
                        expected=0 if unmapped else 0x11223344,
                        label=f'raw {operation} pool={index} offset={offset} log={log}'))
    manifest = args.work / 'cases.json'
    manifest.write_text(json.dumps(cases, indent=2) + '\n')
    print(manifest)


if __name__ == '__main__':
    main()
