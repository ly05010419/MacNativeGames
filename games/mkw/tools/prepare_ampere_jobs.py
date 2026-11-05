#!/usr/bin/env python3
"""Prepare nxnative dmsh_xlate SM86 jobs from private MKW_SHADER_DIR captures.

The output contains game shader code: keep it private. Pass jobs.json and the output
folder to dmsh_xlate. Task/mesh jobs require nxnative PR #983 (b124b24).
The NVN2 control layout and stage decoding follow its dmsh_ampere.py and shader_split.py.
"""
import argparse
import json
import struct
from pathlib import Path
import dmsh_meshstage


def make_job(control, area, key, output):
    if len(control) < 0xbc or struct.unpack_from('<II', control) != (0x98761234, 0xc9):
        raise ValueError('not a complete NVN2 control section')
    inst = struct.unpack_from('<I', control, 0x38)[0]
    c1_start, end = struct.unpack_from('<II', control, 0x40)
    if not (0 < inst <= c1_start <= end == len(area)):
        raise ValueError('invalid instruction/constant segment lengths (capture may be truncated)')
    if control[0x56] in (6, 7):
        return make_mesh_job(control, area, key, output, inst, c1_start, end)
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


def make_mesh_job(control, area, key, output, inst, c1_start, end):
    """PR #983's stage-memory lowering; requires its external Ampere frontend."""
    if inst < 0x90 or inst % 16:
        raise ValueError('mesh instruction area lacks a complete SPH/instruction')
    kind = 'ts' if control[0x56] == 6 else 'ms'
    code_bytes = area[0x80:inst]
    info = {'kind': kind, 'threads': struct.unpack_from('<H', control, 0x58)[0],
            'payload': struct.unpack_from('<H', control, 0x5a)[0]}
    if not 0 < info['threads'] <= 1024:
        raise ValueError('invalid mesh workgroup size')
    if any(struct.unpack_from('<H', code_bytes, at)[0] & 0xfff in (0x984, 0x388, 0x988, 0x38c, 0x98c, 0xf8c)
           for at in range(0, len(code_bytes), 16)):
        raise ValueError('task/mesh shader with its own shared memory is unsupported')
    if kind == 'ms':
        psph = dmsh_meshstage.prim_sph(control, area)
        info.update(max_v=struct.unpack_from('<H', control, 0x60)[0], max_p=struct.unpack_from('<H', control, 0x62)[0],
                    comps=dmsh_meshstage.sph_outputs(area[:0x80]), task=bool(control[0x5e]),
                    pcomps=[v for v in dmsh_meshstage.sph_outputs(psph) if v[0] is not None] if psph else [])
        if not (0 < info['max_v'] <= 256 and 0 < info['max_p'] <= 256):
            raise ValueError('mesh output counts exceed Metal limits')
    layout, words = dmsh_meshstage.layout(kind, info.get('max_v', 0), info.get('comps', []), info['payload'],
                                        info.get('max_p', 0), info.get('pcomps', []))
    if words * 4 > 32768:
        raise ValueError('mesh stage memory exceeds Metal threadgroup limit')
    info['mesh_layout'] = layout
    code = output / (key + '.code'); code.write_bytes(code_bytes)
    return {'key': key, 'stage': 'cs', 'arch': 'sm86', 'code': str(code.resolve()), 'c1': area[c1_start:end].hex(),
            'local': [info['threads'], 1, 1], 'shared': words * 4, 'mesh_layout': layout, 'mesh_stage': info}


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
