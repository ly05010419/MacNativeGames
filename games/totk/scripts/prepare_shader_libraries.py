#!/usr/bin/env python3
"""Incrementally compile this game's translated cache into persistent Metal libraries.

The verified shader inventory selects the current game's raw shader keys. Only complete
MSL/reflection pairs from the current translation chain are considered. Libraries are
addressed by the exact source SHA-256; the active manifest is published atomically after
compilation. Keep the output and reports outside the source checkout. This tool never
reads game keys or modifies the input cache, App, or emulator data.
"""
import argparse
import concurrent.futures
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from build_app import METAL_FLAGS, metal_compiler  # noqa: E402

# Follow the production translation chain, including future cache invalidations.
CHAIN_HEADER = Path(__file__).resolve().parents[1]/'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
CHAIN = re.search(r'#define DMSH_CHAIN "([^"\n]+)"', CHAIN_HEADER.read_text())[1]
HEX = re.compile(r'[0-9a-f]{64}\Z')
SOURCE = re.compile(r'([0-9a-f]{64})(?:-[0-9a-f]{12})?_b\.metal\Z')
KINDS = {'vs': 'vertex', 'ps': 'fragment', 'cs': 'kernel'}
OPTIONS = {'math': 'safe', 'flags': METAL_FLAGS, 'target': 'compiler-default'}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return (json.dumps(value, sort_keys=True, separators=(',', ':'))+'\n').encode()


def read_inventory(path):
    inventory = json.loads(Path(path).read_text())
    if not isinstance(inventory, dict):
        raise ValueError('shader inventory must be an object')
    keys = inventory.get('keys')
    if (inventory.get('schema') != 1 or inventory.get('chain') != CHAIN or
            not isinstance(inventory.get('version'), str) or not inventory['version'] or
            not isinstance(inventory.get('source_romfs_sha256'), str) or
            not HEX.fullmatch(inventory['source_romfs_sha256']) or not isinstance(keys, list) or
            any(not isinstance(k, str) or not HEX.fullmatch(k) for k in keys) or keys != sorted(set(keys))):
        raise ValueError('expected a schema 1 verified shader inventory for '+CHAIN)
    stages = inventory.get('stages', {})
    if not isinstance(stages, dict) or any(k not in keys or v not in KINDS for k, v in stages.items()):
        raise ValueError('invalid stages in shader inventory')
    return inventory


def collect_sources(cache, inventory):
    """Snapshot exact bytes, so a concurrently written cache cannot change a hash's input."""
    keys, stages = set(inventory['keys']), inventory.get('stages', {})
    sources, selected, unrelated, invalid = {}, 0, 0, []
    for path in sorted(Path(cache).glob('*_b.metal')):
        match = SOURCE.fullmatch(path.name)
        if not match or match[1] not in keys:
            unrelated += 1
            continue
        try:
            if path.is_symlink() or path.with_suffix('.json').is_symlink():
                raise ValueError('symlink shader pair')
            reflection = json.loads(path.with_suffix('.json').read_text())
            stage = reflection.get('stage')
            if reflection.get('chain') != CHAIN or reflection.get('key') != path.stem:
                raise ValueError('shader key or translation chain mismatch')
            if stage not in KINDS or stages.get(match[1], stage) != stage:
                raise ValueError('shader stage does not match the current inventory')
            if reflection.get('msl_entry_kind') != KINDS[stage]:
                raise ValueError(f'{stage} reflection requires {KINDS[stage]} MSL, got '
                                 f"{reflection.get('msl_entry_kind')!r}")
            data = path.read_bytes()
            if not data.strip():
                raise ValueError('empty Metal source')
        except (OSError, ValueError, TypeError, AttributeError) as error:
            invalid.append({'source': path.name, 'error': str(error)})
            continue
        sources.setdefault(digest(data), data)
        selected += 1
    return sources, {'selected_pairs': selected, 'unique_sources': len(sources),
                     'unrelated_sources': unrelated, 'invalid_pairs': invalid}


def observed_keys(cache, inventory):
    """Only observations with this game's exact identity can extend its static inventory."""
    path = Path(cache)/'observed'/(inventory['source_romfs_sha256']+'.keys')
    try:
        lines = path.read_text().splitlines()
        identity = {k: inventory[k] for k in ('schema', 'chain', 'version', 'source_romfs_sha256')}
        if not lines or json.loads(lines[0]) != identity:
            return set()
        return {line for line in lines[1:] if HEX.fullmatch(line)}
    except (OSError, ValueError):
        return set()


def compiler_settings(compiler=None):
    if compiler is None:
        found = metal_compiler()
        if not found:
            raise ValueError('no Metal compiler found; select an installed compiler with --compiler')
        compiler, _, environment = found
    else:
        environment = dict(os.environ)
    compiler = str(Path(compiler).resolve())
    result = subprocess.run([compiler, '--version'], capture_output=True, text=True, env=environment)
    if result.returncode or not result.stdout.strip():
        raise ValueError('cannot identify Metal compiler: '+(result.stderr or result.stdout).strip())
    identity = dict(executable=compiler, version=result.stdout.strip(), platform=sys.platform,
                    machine=platform.machine(), platform_release=platform.release())
    directory = digest(encoded({'compiler_identity': identity, 'options': OPTIONS}))
    return compiler, environment, identity, directory


def library_present(path):
    """Metal rejects damaged libraries too; exclude incomplete local outputs before publication."""
    if path.is_symlink() or not path.is_file():
        return False
    with path.open('rb') as stream:
        return stream.read(4) == b'MTLB'


def existing_libraries(output, identity, directory):
    path = output/'manifest.json'
    if not path.is_file():
        return {}
    old = json.loads(path.read_text())
    if not isinstance(old, dict):
        raise ValueError('library manifest must be an object')
    if (old.get('schema') != 1 or old.get('chain') != CHAIN or
            old.get('compiler_identity') != identity or old.get('options') != OPTIONS or
            old.get('directory') != directory):
        return {}  # another compiler/target has its own directory; its files stay untouched
    libraries = old.get('libraries')
    if not isinstance(libraries, dict):
        raise ValueError('invalid existing library manifest')
    result = {}
    for sha, relative in libraries.items():
        if not isinstance(sha, str) or not HEX.fullmatch(sha) or relative != f'{directory}/{sha}.metallib':
            raise ValueError('unsafe path in existing library manifest')
        if library_present(output/relative):
            result[sha] = relative
    return result


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='.'+path.name+'-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(encoded(value))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def prepare(cache, inventory_path, output, *, app_libraries=None, compiler=None, jobs=1, dry_run=False):
    started = time.monotonic()
    if jobs < 1:
        raise ValueError('jobs must be positive')
    if not Path(cache).is_dir():
        raise ValueError('shader cache directory does not exist')
    inventory = read_inventory(inventory_path)
    observed = observed_keys(cache, inventory) - set(inventory['keys'])
    inventory = {**inventory, 'keys': sorted(set(inventory['keys']) | observed)}
    sources, counts = collect_sources(cache, inventory)
    compiler, environment, identity, directory = compiler_settings(compiler)
    output = Path(output).resolve()
    report = dict(schema=1, chain=CHAIN, version=inventory['version'],
                  source_romfs_sha256=inventory['source_romfs_sha256'], compiler_identity=identity,
                  options=OPTIONS, directory=directory, dry_run=dry_run, **counts)
    report['observed_keys'] = len(observed)
    def run():
        libraries = existing_libraries(output, identity, directory)
        bundled, reused, todo = 0, 0, {}
        for sha, data in sources.items():
            if app_libraries and library_present(Path(app_libraries)/(sha+'.metallib')):
                bundled += 1
            elif library_present(output/directory/(sha+'.metallib')):
                libraries[sha] = f'{directory}/{sha}.metallib'
                reused += 1
            else:
                todo[sha] = data
        report.update(bundled_sources=bundled, reused_sources=reused, pending_sources=len(todo),
                      compiled_sources=0, failures=[])
        if dry_run:
            return
        folder = output/directory
        if folder.is_symlink():
            raise ValueError('compiler directory must not be a symlink')
        folder.mkdir(exist_ok=True)
        module_cache = folder/'module-cache'
        if module_cache.is_symlink():
            raise ValueError('module cache must not be a symlink')

        def compile_one(item):
            sha, data = item
            with tempfile.TemporaryDirectory(prefix='.compile-', dir=folder) as work:
                source, partial = Path(work)/(sha+'.metal'), Path(work)/(sha+'.metallib')
                source.write_bytes(data)
                result = subprocess.run([compiler, *METAL_FLAGS, '-fmodules-cache-path='+str(module_cache),
                                         '-o', str(partial), str(source)],
                                        capture_output=True, text=True, env=environment)
                if result.returncode or not library_present(partial):
                    return sha, (result.stdout+result.stderr).strip() or 'compiler did not produce a Metal library'
                os.replace(partial, folder/(sha+'.metallib'))
                return sha, None

        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
            for sha, error in pool.map(compile_one, todo.items()):
                if error:
                    report['failures'].append({'source_sha256': sha, 'error': error})
                else:
                    libraries[sha] = f'{directory}/{sha}.metallib'
                    report['compiled_sources'] += 1
        manifest = dict(schema=1, chain=CHAIN, compiler_identity=identity, options=OPTIONS,
                        directory=directory, libraries=libraries)
        atomic_json(output/'manifest.json', manifest)
        report['manifest_libraries'] = len(libraries)

    if dry_run:
        run()
    else:
        output.mkdir(parents=True, exist_ok=True)
        with (output/'.prepare.lock').open('a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as error:
                raise ValueError('another shader library preparation is running') from error
            run()
    report['seconds'] = round(time.monotonic()-started, 3)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', required=True, type=Path)
    parser.add_argument('--inventory', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--app-libraries', type=Path, help='skip exact source hashes already bundled in the App')
    parser.add_argument('--compiler', type=Path, help='installed Metal compiler; otherwise use build_app discovery')
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--dry-run', action='store_true', help='report coverage without creating output files')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    report = prepare(args.cache, args.inventory, args.output, app_libraries=args.app_libraries,
                     compiler=args.compiler, jobs=args.jobs, dry_run=args.dry_run)
    if args.report:
        atomic_json(args.report, report)
    print(json.dumps(report, sort_keys=True))
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print('ERROR: '+str(error), file=sys.stderr)
        sys.exit(1)
