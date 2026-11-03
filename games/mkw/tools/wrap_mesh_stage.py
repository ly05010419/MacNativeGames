#!/usr/bin/env python3
"""Wrap an SM86 stage-memory compute translation as a Metal object/mesh function.

Usage: wrap_mesh_stage.py job.json shader.metal
The JSON is a prepare_ampere_jobs job containing mesh_stage metadata.
Input and output shader sources contain private game data.
"""
import json
from pathlib import Path
import sys

import dmsh_meshstage


def wrap(job, source):
    info = dict(job['mesh_stage'])
    if info['kind'] not in ('ts', 'ms'):
        raise ValueError('unknown mesh stage kind')
    # JSON arrays must recover tuples for component lookup in the upstream wrapper.
    for name in ('comps', 'pcomps'):
        if name in info:
            info[name] = [tuple(component) for component in info[name]]
    return dmsh_meshstage.to_stage(source, info['kind'], info)


if __name__ == '__main__':
    prepare = sys.argv[1] == '--prepare'
    job = json.loads(Path(sys.argv[2] if prepare else sys.argv[1]).read_text())
    if isinstance(job, list):
        if len(job) != 1:
            raise ValueError('wrapper requires one translation job')
        job = job[0]
    if prepare:
        from prepare_ampere_jobs import make_job
        control = bytes.fromhex(job['mesh_control'])
        if job['stage'] != {6: 'ts', 7: 'ms'}.get(control[0x56]):
            raise ValueError('request and control mesh stage differ')
        prepared = make_job(control, Path(job['code']).read_bytes(), job['variant'] + '.stage', Path(sys.argv[3]))
        job.update(prepared)
        print(json.dumps(job))
    else:
        source = Path(sys.argv[2])
        source.write_text(wrap(job, source.read_text()))
