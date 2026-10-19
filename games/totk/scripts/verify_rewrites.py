#!/usr/bin/env python3
"""Run/record the phase 1+2 validation gates against exact local module files.

--collect-existing records the already-run v3 gates without repeating them.
Logs and manifests contain no key values and remain excluded from Git.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def gate(command, output, environment):
    with output.open('w') as stream:
        result = subprocess.run(command, cwd=ROOT, env=environment,
                                stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f'Gate failed ({result.returncode}); see {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', choices=('v0','v112','v121','v143'), default='v0')
    parser.add_argument('--collect-existing', action='store_true')
    parser.add_argument('--jobs', type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    version = args.version
    analysis = ROOT / ('analysis' if version == 'v0' else f'analysis/{version}-port')
    analysis.mkdir(parents=True, exist_ok=True)
    vendor = ROOT / 'third_party/d2r'
    outputs = ROOT / f'rewritten/{version}'
    source_hash = digest(vendor / 'patch_nso.py')
    if (outputs / 'build_src.sha256').read_text().split()[0] != source_hash:
        raise RuntimeError('Rewrite source changed since artifacts were generated; rebuild first')
    environment = os.environ.copy()
    environment.setdefault('LLVM_BIN', '/opt/homebrew/opt/llvm/bin')
    environment['DM_VCACHE'] = str(analysis / 'llvm-cache')
    inputs = [ROOT / f'extracted/{version}/exefs/{module}' for module in ['main', 'sdk', 'subsdk0']]
    binaries = [outputs / f'{module}.patched.bin' for module in ['main', 'sdk', 'subsdk0']]
    fingerprints = {str(path.relative_to(ROOT)): digest(path) for path in inputs + binaries}
    extraction_manifest = ROOT / f'extracted/{version}/extraction-manifest.json'
    extracted = {entry['name']: entry['sha256'] for entry in json.loads(extraction_manifest.read_text())['files']}
    for path in inputs:
        if fingerprints[str(path.relative_to(ROOT))] != extracted[path.name]:
            raise RuntimeError(f'Module differs from verified ExeFS extraction: {path.name}')
    modules = {}
    for module in ['main', 'sdk', 'subsdk0']:
        nso = ROOT / f'extracted/{version}/exefs/{module}'
        structure = analysis / f'{module}-structure-v3.log'
        equivalence = analysis / f'{module}-equivalence-v3.log'
        if not args.collect_existing:
            gate([sys.executable, str(vendor / 'structck.py'), str(outputs), module, str(nso)], structure, environment)
            gate([sys.executable, str(vendor / 'equiv.py'), str(outputs), module, str(nso),
                  '--jobs', str(args.jobs), '--min-eff', '2', '--max-extra', '16',
                  '--mutate', '300' if module == 'main' else '150',
                  '--tsv', str(analysis / f'{module}-equivalence-v3.tsv')], equivalence, environment)
        structural = re.search(rf'^STRUCT {module} OK 改写点=(\d+)', structure.read_text(), re.M)
        semantic = re.search(rf'^EQUIV {module} OK 站点=(\d+) 试验=(\d+) 有效试验=(\d+) 双异常=(\d+).*变异=(\d+).*抓到=(\d+) 漏抓=0', equivalence.read_text(), re.M)
        if not structural or not semantic:
            raise RuntimeError(f'Missing successful structure/equivalence gate for {module}')
        values = list(map(int, semantic.groups()))
        if values[4] != values[5]:
            raise RuntimeError(f'Missed mutation controls for {module}')
        modules[module] = dict(structural_sites=int(structural[1]), semantic_sites=values[0],
                               trials=values[1], effective_trials=values[2], both_fault_trials=values[3],
                               detected_negative_controls=values[5],
                               log_hashes={structure.name: digest(structure), equivalence.name: digest(equivalence)})
    call_log = analysis / 'call-window-tests.log'
    atomic_json = analysis / 'exclusive-window-tests.json'
    if not args.collect_existing:
        gate([sys.executable, str(ROOT / 'scripts/test_call_window.py')], call_log, environment)
        if version == 'v143':
            gate([sys.executable, str(ROOT / 'scripts/test_v143_atomics.py')], atomic_json, environment)
        else:
            gate([sys.executable, str(ROOT / 'scripts/test_exclusive_windows.py'), str(outputs), str(inputs[0]), "--version", version], atomic_json, environment)
        gate(['make', 'probe-call'], analysis / 'native-call-build.log', environment)
    if not re.search(r'^OK$', call_log.read_text(), re.M):
        raise RuntimeError('Prepared-call differential tests did not pass')
    atomic = json.loads(atomic_json.read_text())
    native = json.loads((ROOT / 'analysis/native-call-probe.json').read_text())
    if version == 'v143':
        if (atomic['status'] != 'pass' or atomic['negative_controls_detected'] != 1
                or sum(case['increments'] for case in atomic['cases']) != 320000
                or any(case['status'] != 'pass' or case['threads'] != 8 for case in atomic['cases'])
                or atomic['source_sha256'] != digest(ROOT / 'scripts/native_exclusive_probe.c')):
            raise RuntimeError('Native 1.4.3 atomic-window checks did not pass')
    elif (atomic['status'] != 'pass' or not atomic['windows']
          or atomic['wrong_exit_negative_controls_detected'] != len(atomic['windows'])):
        raise RuntimeError('Atomic-window regression tests did not pass')
    if native['status'] != 'pass' or native['completed_calls'] != 20000 or native['errors'] != 0:
        raise RuntimeError('Native prepared-call hardware test did not pass')
    for path in inputs + binaries:
        if fingerprints[str(path.relative_to(ROOT))] != digest(path):
            raise RuntimeError(f'Input/output changed during verification: {path}')
    source_files = [vendor / name for name in ['patch_nso.py', 'call_window.py', 'structck.py', 'equiv.py', 'nsoimg.py', 'relr.py']]
    source_files += [ROOT / 'scripts' / name for name in ['test_call_window.py', 'test_exclusive_windows.py', 'native_call_probe.c', 'emit_native_call_fixture.py', 'verify_rewrites.py']]
    if version == 'v143': source_files += [ROOT / 'scripts' / name for name in ['test_v143_atomics.py', 'native_exclusive_probe.c']]
    summary = {
        'format': 'totk-native-phase12-validation-v1', 'status': 'pass',
        'recorded_at': datetime.now(timezone.utc).isoformat(), 'rom_version': version,
        'modules': modules, 'module_file_sha256': fingerprints,
        'validation_source_sha256': {str(p.relative_to(ROOT)): digest(p) for p in source_files},
        'extraction_manifest_sha256': digest(extraction_manifest),
        'native_fixture_sha256': digest(ROOT / 'build/native-call-fixture.bin'),
        'native_probe_executable_sha256': digest(ROOT / 'build/native_call_probe'),
        'call_window': {'differential_cases': 536, 'negative_controls': 4, 'native': native},
        'exclusive_windows': atomic,
        'totals': {key: sum(m[key] for m in modules.values()) for key in
                   ['structural_sites', 'semantic_sites', 'trials', 'effective_trials', 'both_fault_trials', 'detected_negative_controls']},
        'scope': ['All listed module rewrite sites checked structurally.',
                  'Full per-site differential verification uses the upstream deexcl model for exclusive accesses.',
                  'Real exclusive-success and injected-failure behavior separately checked on every w18 external-exit window (two in 1.0.0 and 1.1.2, one in 1.2.1).',
                  'The prepared-call window requires no alternate entry at the original call word; known branches/functions/relocations are checked.',
                  'Darwin TSD hardware checks are for this Mac; this is not a proof for all Apple devices.',
                  '100 SDK system/timer gates are structurally checked; host service semantics are not implemented or tested.',
                  'Game startup, concurrency correctness, graphics and gameplay remain unverified.'],
    }
    report = analysis / 'phase12-validation.json'
    report.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({'status': summary['status'], 'totals': summary['totals'], 'report': str(report)}, indent=2))


if __name__ == '__main__':
    main()
