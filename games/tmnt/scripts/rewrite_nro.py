#!/usr/bin/env python3
"""Experimentally rewrite a privately prepared NRO; never execute guest code."""
import argparse
import importlib.metadata
import json
import os
import subprocess
import sys
from pathlib import Path

from extract_exefs import validate_private_output
from rewrite_modules import GAME, OUTPUTS, TOOL_SOURCES, sha256


def verify_nro_report(prepared, rewritten):
    manifest = json.loads((prepared / 'manifest.json').read_text())
    report = json.loads((rewritten / 'report.json').read_text())
    if manifest.get('status') != 'pass' or report.get('exit_code') != 0:
        raise ValueError('NRO preparation or rewrite failed')
    inputs = {'nro': sha256(prepared / 'module.nro'), 'nso': sha256(prepared / 'synthetic.nso')}
    if inputs != {'nro': manifest['nro_sha256'], 'nso': manifest['synthetic_nso_sha256']} or inputs != report.get('input_sha256'):
        raise ValueError('prepared NRO inputs differ from report')
    tool_dir = GAME / 'third_party/d2r'
    if (report.get('tools') != {name: sha256(tool_dir / name) for name in TOOL_SOURCES} or
        report.get('dependencies') != {name: importlib.metadata.version(name) for name in ('capstone', 'lz4')} or
        report.get('python') != sys.version.split()[0] or
        report.get('options') != {'ruleset': 'native', 'private_gates': False}):
        raise ValueError('NRO rewrite tool or options differ from report')
    outputs = report.get('outputs', {})
    if set(outputs) != set(OUTPUTS) or any(sha256(rewritten / f'nro.{suffix}') != digest for suffix, digest in outputs.items()):
        raise ValueError('NRO rewrite outputs differ from report')
    return report


def rewrite(prepared, out):
    validate_private_output(out)
    manifest = json.loads((prepared / 'manifest.json').read_text())
    if manifest.get('status') != 'pass':
        raise ValueError('NRO preparation was not verified')
    nro, nso = prepared / 'module.nro', prepared / 'synthetic.nso'
    before = {'nro': sha256(nro), 'nso': sha256(nso)}
    if before != {'nro': manifest['nro_sha256'], 'nso': manifest['synthetic_nso_sha256']}:
        raise ValueError('prepared module differs from its manifest')
    tool = GAME / 'third_party/d2r/patch_nso.py'
    tools = {name: sha256(tool.parent / name) for name in TOOL_SOURCES}
    dependencies = {name: importlib.metadata.version(name) for name in ('capstone', 'lz4')}
    out.mkdir(parents=True)
    with (out / 'rewrite.log').open('w') as log:
        result = subprocess.run([sys.executable, str(tool), str(nso), str(out), 'nro'],
            stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0'))
    if ({'nro': sha256(nro), 'nso': sha256(nso)} != before or
        tools != {name: sha256(tool.parent / name) for name in TOOL_SOURCES}):
        raise ValueError('input or rewriter changed during processing')
    report = {'input_sha256': before, 'tools': tools, 'dependencies': dependencies,
              'python': sys.version.split()[0], 'options': {'ruleset': 'native', 'private_gates': False},
              'exit_code': result.returncode}
    if result.returncode == 0:
        try:
            report['outputs'] = {suffix: sha256(out / f'nro.{suffix}') for suffix in OUTPUTS}
        except FileNotFoundError:
            report['exit_code'] = 1
            report['error'] = 'rewriter did not produce every required artifact'
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return int(report['exit_code'] != 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prepared', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    status = rewrite(args.prepared.resolve(), args.out.resolve())
    print('NRO converted; validation still required' if status == 0 else 'NRO rewrite failed; see private log')
    return status


if __name__ == '__main__':
    raise SystemExit(main())
