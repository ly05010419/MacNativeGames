#!/usr/bin/env python3
"""Prepare nxnative dmsh_xlate SM86 jobs from private MKW_SHADER_DIR captures.

The output contains game shader code: keep it private. Pass jobs.json and the output
folder to dmsh_xlate from nxnative commit 65501fbbaf8526f7aab0a1edd028135888590f8e.
The NVN2 control layout and stage decoding follow its dmsh_ampere.py and shader_split.py.
"""
import argparse
import json
import struct
from pathlib import Path


def make_job(control, area, key, output):
    if len(control) < 0xbc or struct.unpack_from('<II', control) != (0x98761234, 0xc9):
        raise ValueError('not a complete NVN2 control section')
    inst = struct.unpack_from('<I', control, 0x38)[0]
    c1_start, end = struct.unpack_from('<II', control, 0x40)
    if not (0 < inst <= c1_start <= end == len(area)):
        raise ValueError('invalid instruction/constant segment lengths (capture may be truncated)')
    if control[0x56] == 5:
        stage = 'cs'
    elif len(area) >= 4:
        stage = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}.get((struct.unpack_from('<I', area)[0] >> 10) & 15)
    else:
        stage = None
    if stage not in ('vs', 'ps', 'cs'):
        raise ValueError(f'unsupported or unknown stage {stage!r}')
    code = output / (key + '.code')
    code.write_bytes(area[:inst])
    job = {'key': key, 'stage': stage, 'arch': 'sm86', 'code': str(code.resolve()), 'c1': area[c1_start:end].hex()}
    if stage == 'cs':
        job['local'] = list(struct.unpack_from('<III', control, 0x58))
        job['nvn2_cs'] = [control[0xb1], control[0xb2], control[0xb3], struct.unpack_from('<H', control, 0xba)[0], control[0xa9] * 4]
    return job


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('captures', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    jobs, errors = [], []
    for ctl in sorted(args.captures.glob('*.ctl')):
        try:
            jobs.append(make_job(ctl.read_bytes(), ctl.with_suffix('.bin').read_bytes(), ctl.stem, args.output))
        except (ValueError, OSError, struct.error) as e:
            errors.append(f'{ctl.name}: {e}')
    (args.output / 'jobs.json').write_text(json.dumps(jobs, indent=2) + '\n')
    for error in errors:
        print(error)
    print(f'{len(jobs)} jobs prepared; {len(errors)} rejected')
    return bool(errors) or not jobs


if __name__ == '__main__':
    raise SystemExit(main())
