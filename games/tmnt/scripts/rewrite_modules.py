#!/usr/bin/env python3
"""Experimentally rewrite verified base modules into a new private directory."""
import argparse
import hashlib
import importlib.metadata
import json
import os
import subprocess
import sys
from pathlib import Path
from expected_inputs import check_supported_files

GAME = Path(__file__).resolve().parents[1]
REPO = GAME.parent.parent
MODULES = ('main', 'sdk', 'subsdk0', 'subsdk1', 'subsdk2')
TITLE = '0100FE701475A000'
TOOL_SOURCES = ('patch_nso.py', 'call_window.py', 'relr.py', 'tmnt_graphics_rules.py')
OUTPUTS = ('patched.bin', 'fixups.tsv', 'gates.tsv', 'sites.tsv', 'cuts.tsv', 'obs.tsv', 'params.tsv')
MAIN_RULE_OUTPUT = 'rules.tsv'


def sha256(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def verify_exefs(exefs):
    manifest = json.loads((exefs.parent / 'extraction-manifest.json').read_text())
    if manifest['program']['title_id'] != TITLE:
        raise ValueError('manifest is not the supported base game')
    hashes = {item['name']: item['sha256'] for item in manifest['files']}
    check_supported_files(manifest['program']['title_id'], manifest['files'])
    for name in (*MODULES, 'main.npdm'):
        if sha256(exefs / name) != hashes.get(name):
            raise ValueError('extracted input differs from manifest')
    return hashes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exefs', required=True, type=Path)
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    exefs, out = args.exefs.resolve(), args.out.resolve()
    if out == REPO or out.is_relative_to(REPO):
        parser.error('output must be outside the repository')
    expected = verify_exefs(exefs)
    out.mkdir(parents=True, exist_ok=False)
    tool = GAME / 'third_party/d2r/patch_nso.py'
    tool_hashes = {name: sha256(tool.parent / name) for name in TOOL_SOURCES}
    dependencies = {name: importlib.metadata.version(name) for name in ('capstone', 'lz4')}
    report = {}
    for module in MODULES:
        with (out / (module + '.log')).open('w') as log:
            result = subprocess.run([sys.executable, str(tool), str(exefs / module), str(out), module],
                                    stdout=log, stderr=subprocess.STDOUT,
                                    env=dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0',
                                             TMNT_GRAPHICS_GATES='1'))
        if sha256(exefs / module) != expected[module] or tool_hashes != {name: sha256(tool.parent / name) for name in TOOL_SOURCES}:
            raise ValueError('input or rewriter changed during processing')
        entry = {'input_sha256': expected[module], 'exit_code': result.returncode}
        if result.returncode == 0:
            try:
                entry['outputs'] = {suffix: sha256(out / f'{module}.{suffix}') for suffix in OUTPUTS}
                if module == 'main':
                    entry['outputs'][MAIN_RULE_OUTPUT] = sha256(out / f'main.{MAIN_RULE_OUTPUT}')
            except FileNotFoundError:
                entry['exit_code'] = 1
                entry['error'] = 'rewriter did not produce every required artifact'
        report[module] = entry
        print(module, 'converted' if entry['exit_code'] == 0 else 'failed; see private log', flush=True)
    (out / 'report.json').write_text(json.dumps({'tools': tool_hashes, 'dependencies': dependencies,
        'python': sys.version.split()[0], 'options': {'ruleset': 'native', 'private_gates': False,
                                                    'tmnt_graphics_gates': True},
        'modules': report}, indent=2) + '\n')
    return int(any(entry['exit_code'] for entry in report.values()))


if __name__ == '__main__':
    raise SystemExit(main())
