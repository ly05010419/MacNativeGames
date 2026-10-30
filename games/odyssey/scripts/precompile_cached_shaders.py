#!/usr/bin/env python3
"""Compile locally translated Odyssey shaders before the next app launch.

The input and libraries are game-derived. Both stay under ignored Odyssey_build.
The runtime looks up each library by SHA-256 of the exact MSL source bytes.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import subprocess

GAME = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('ODYSSEY_WORK', GAME / 'Odyssey_build')).resolve() / 'build'   # ODYSSEY_WORK: another build folder
CACHE = BUILD / 'shader-cache/rt-totk-native-v4-unverified'
OUTPUT = BUILD / 'shader-libs'
FLAGS = ('-fmetal-math-mode=safe',)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--cache', type=Path, default=CACHE,
                        help='folder of translated *_b.metal variants (default: the runtime cache)')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs must be between 1 and 8')
    env = dict(os.environ, DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer'))
    sources = sorted(args.cache.glob('*_b.metal'))
    if not sources:
        raise SystemExit('No translated local shader variants found')
    found = subprocess.run(['xcrun', '--find', 'metal'], env=env, text=True,
                           capture_output=True)
    if found.returncode:
        raise SystemExit('Offline Metal compiler unavailable. Finish installing the Xcode MetalToolchain component, then retry.')
    compiler = found.stdout.strip()
    check = subprocess.run([compiler, '--version'], env=env, text=True,
                           capture_output=True)
    if check.returncode or not check.stdout.splitlines():
        raise SystemExit('Offline Metal compiler is not ready. Finish installing the Xcode MetalToolchain component, then retry.')
    version = check.stdout.splitlines()[0]
    identity = ' '.join((version, *FLAGS))
    OUTPUT.mkdir(parents=True, exist_ok=True)
    stamp = OUTPUT / 'compiler.txt'
    if stamp.is_file() and stamp.read_text() != identity:
        raise SystemExit('Metal compiler identity changed; inspect the existing local library cache')
    stamp.write_text(identity)
    todo = {}
    for source in sources:
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        library = OUTPUT / (digest + '.metallib')
        if not library.is_file() or library.stat().st_size < 4:
            todo.setdefault(digest, source)
    print(f'Found {len(sources)} local variants; compiling {len(todo)} missing libraries with {args.jobs} workers.', flush=True)

    def compile_one(item):
        digest, source = item
        partial = OUTPUT / (digest + '.partial')
        library = OUTPUT / (digest + '.metallib')
        result = subprocess.run([compiler, *FLAGS, '-o', str(partial), str(source)],
                                env=env, text=True, capture_output=True)
        if result.returncode or not partial.is_file() or partial.read_bytes()[:4] != b'MTLB':
            partial.unlink(missing_ok=True)
            return f'{source.name}: {result.stdout}{result.stderr}\n'
        partial.replace(library)
        return None

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        failures = [message for message in pool.map(compile_one, todo.items()) if message]
    (OUTPUT / 'compile-failures.log').write_text(''.join(failures))
    print(f'Precompiled {len(todo) - len(failures)} libraries; {len(failures)} failed (local log).', flush=True)
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
