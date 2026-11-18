#!/usr/bin/env python3
"""Extract the base game + update 1.0.1 from the player's containers into a new folder, with nx_extract's checks: the programs chosen by their content metas, header signatures,
each NCA's SHA-256 against its meta, and every file through the RomFS hash tree. result.json in the evidence folder records what passed; the build takes only the folder this makes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

from ss_env import environment

GAME = Path(__file__).resolve().parents[1]
TITLE = '01002DA013484000'
LIBHAC_SHA256 = '1431267861db5ff68216315b164f10c0edab4df162cedf5429bf8a143b76de19'


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def prepare_offline_dependencies(work, package):
    """Accept an explicit local prerequisite; never download or use a global cache."""
    package = package.expanduser().resolve()
    if not package.is_file():
        raise ValueError('local LibHac 0.19.0 package is missing; see README.md')
    if sha256(package) != LIBHAC_SHA256:
        raise ValueError('LibHac 0.19.0 package SHA-256 mismatch')
    feed = work/'dependencies/nuget'
    feed.mkdir(parents=True, exist_ok=True)
    shutil.copy2(package, feed/'libhac.0.19.0.nupkg')
    return feed


def build_extractor(work, env):
    """nx_extract from this folder's copy, built outside the checkout."""
    project, out = work/'tmp/nx_extract', work/'tools/nx_extract'
    shutil.rmtree(project, ignore_errors=True); shutil.rmtree(out, ignore_errors=True)
    shutil.copytree(GAME/'runtime/tools/nx_extract', project, ignore=shutil.ignore_patterns('bin', 'obj'))
    subprocess.run(['dotnet', 'restore', str(project), '--configfile', str(project/'NuGet.Config'), '--source', env['DM_NUGET_SOURCE'],
                    '-p:NuGetAudit=false', '-p:RestoreIgnoreFailedSources=false', '--force', '-v', 'minimal', '-nologo'], env=env, check=True)
    subprocess.run(['dotnet', 'build', str(project), '--no-restore', '-c', 'Release', '-o', str(out), '-v', 'quiet', '-nologo'], env=env, check=True)
    if not (out/'nx_extract').is_file():
        raise ValueError('nx_extract built without an executable')
    return out/'nx_extract'


def tolerated_meta(container_sha256):
    """The base meta whose invalid signature may be accepted: only for the one container the exception names."""
    entry = json.loads((GAME/'assets/integrity-exceptions.json').read_text())['base_meta']
    return entry['meta_sha256'] if entry and container_sha256 == entry['container_sha256'] else None


def summary(code, text, inputs, update, tolerance):
    """What the extractor's log says, line by line; each NCA's signature and each program's hashes are kept apart, and only what ran is listed as performed.
    Pass needs every signature valid but a base meta tolerated by its SHA-256, both hashes equal, no damaged file, and files written."""
    wanted = ('base game', 'update') if update else ('base game',)
    selected = dict(re.findall(r'^\[select\] (base game|update): ([0-9a-f]{32}) ', text, re.M))
    signatures = {f'{what} {kind}': dict(result=result, sha256=sha or None, tolerated=False) for what, kind, result, sha in
                  re.findall(r'^\[signature\] (base game|update) (meta|program): (\w+)(?: sha256 ([0-9a-f]{64}))?$', text, re.M)}
    for what in re.findall(r'^\[tolerated\] (base game) meta:', text, re.M):
        signatures[f'{what} meta'].update(tolerated=True, exception='assets/integrity-exceptions.json')
    hashes = {what: dict(computed=got, expected=want, meta='base meta, signature not valid' if signatures.get('base game meta', {}).get('tolerated') and what == 'base game' else f'{what} meta, signature valid')
              for what, got, want in re.findall(r'^\[hash\] (base game|update) program: computed ([0-9a-f]{64}) expected ([0-9a-f]{64})$', text, re.M)}
    damaged = re.findall(r'^\[RED\] hash check failed: (.+)$', text, re.M)
    files = sum(int(n) for n in re.findall(r': (\d+) files ', text))
    signed = all(signatures.get(f'{w} {k}', {}).get('result') == 'Valid' or (k == 'meta' and w == 'base game' and signatures.get(f'{w} {k}', {}).get('tolerated')) for w in wanted for k in ('meta', 'program'))
    hashed = all(w in hashes and hashes[w]['computed'] == hashes[w]['expected'] for w in wanted)
    if signatures.get('base game meta', {}).get('tolerated'):   # the exception names the program too: a tolerated meta can select only this one
        entry = json.loads((GAME/'assets/integrity-exceptions.json').read_text())['base_meta']
        hashed = hashed and entry is not None and hashes['base game']['computed'] == entry['program_sha256']   # no exception (this target's case): a tolerance line cannot pass
    ok = code == 0 and not damaged and signed and hashed and files > 0 and all(w in selected for w in wanted)
    performed = ['content metas selected']*bool(selected)+['header signatures']*bool(signatures)+['program SHA-256 against metas']*bool(hashes)+['every file read through ErrorOnInvalid']*(files > 0)
    authenticated = dict(base_version_v0=signatures.get('base game meta', {}).get('result') == 'Valid')
    if update:
        authenticated['update_version_65536'] = signatures.get('update meta', {}).get('result') == 'Valid'
    return dict(status='pass' if ok else 'fail', exit=code, inputs=inputs, selected=selected, signatures=signatures, hashes=hashes, checks_performed=performed,
                metadata_authenticated=authenticated, tolerance_offered=bool(tolerance), damaged=damaged, files=files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True, type=Path, help='the base game: an XCI or NSP')
    parser.add_argument('--update', type=Path, help='update 1.0.1; left out: the base game alone, for the composition evidence')
    parser.add_argument('--keys', required=True, type=Path, help='prod.keys; read by nx_extract, never copied')
    parser.add_argument('--libhac', required=True, type=Path, help='local libhac.0.19.0.nupkg')
    parser.add_argument('--work-dir', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path, help='new folder for ExeFS/ and RomFS/')
    parser.add_argument('--evidence', required=True, type=Path, help='new folder for run.log and result.json')
    args = parser.parse_args()
    work, output, evidence = (p.expanduser().resolve() for p in (args.work_dir, args.output, args.evidence))
    checkout = GAME.parents[1]
    for path in (work, output, evidence):
        if path == checkout or checkout in path.parents:
            parser.error('work, output and evidence must be outside the checkout')
    if output.exists() or evidence.exists():
        parser.error('output and evidence must be new folders')
    env = environment(work, TITLE)
    prepare_offline_dependencies(work, args.libhac)
    extractor = build_extractor(work, env)
    update = args.update.expanduser().resolve() if args.update else None
    sources = [args.base.expanduser().resolve()] + ([update] if update else [])
    inputs = {p.name: dict(size=p.stat().st_size, sha256=sha256(p)) for p in sources}
    tolerance = tolerated_meta(inputs[sources[0].name]['sha256'])
    evidence.mkdir(parents=True)
    with (evidence/'run.log').open('w') as log:
        code = subprocess.run([str(extractor), str(args.keys.expanduser().resolve()), str(sources[0]), str(update or '-'), str(output)]+(['--tolerated-base-meta', tolerance] if tolerance else []),
                              env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    if {p.name: dict(size=p.stat().st_size, sha256=sha256(p)) for p in sources} != inputs:
        raise ValueError('the inputs changed during the extraction')
    result = summary(code, (evidence/'run.log').read_text(errors='replace'), inputs, bool(update), tolerance)
    (evidence/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result[k] for k in ('status', 'exit', 'selected', 'files')}))
    return 0 if result['status'] == 'pass' else 1


if __name__ == '__main__':
    try: sys.exit(main())
    except (ValueError, OSError, subprocess.CalledProcessError) as error: sys.exit(f'error: {error}')
