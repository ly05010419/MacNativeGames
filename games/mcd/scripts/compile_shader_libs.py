#!/usr/bin/env python3
"""Precompile recorded ITT MSL without changing source/reflection or runtime fallback.

Outputs are source-SHA256 named, as required by shader_lib_prebuilt_path. A
compiler/flags identity and output digest prevent reusing stale or partial files.
Missing optional toolchains and individual compile failures keep source fallback.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

CHAIN = 'totk-native-v4-unverified'
FLAGS = ['-fmetal-math-mode=safe']


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def sources(cache):
    found = {}
    for src in sorted(cache.glob('*_b.metal')):
        try:
            reflection = json.loads(src.with_suffix('.json').read_text())
            if reflection.get('chain') != CHAIN or reflection.get('key') != src.stem:
                continue
            raw = src.read_bytes()
        except (OSError, ValueError, AttributeError):
            continue
        found[digest(raw)] = raw
    for src in sorted((cache / 'variants').glob('*.metal')):
        raw = src.read_bytes()
        if digest(raw) == src.stem:
            found[src.stem] = raw
    return found


def compile_cache(cache, output, compiler, identity, jobs=4):
    output.mkdir(parents=True, exist_ok=True)
    manifest = output / 'itt-libraries.json'
    try:
        old = json.loads(manifest.read_text())
    except (OSError, ValueError):
        old = {}
    if not isinstance(old, dict) or old.get('compiler') != identity or old.get('flags') != FLAGS:
        old = {}
    previous = old.get('libraries', {})
    if not isinstance(previous, dict):
        previous = {}

    def compile_one(item):
        key, raw = item
        target = output / (key + '.metallib')
        if target.is_file() and previous.get(key) == digest(target.read_bytes()):
            return key, previous[key], None
        # Never leave a stale same-source library available after a failed rebuild.
        target.unlink(missing_ok=True)
        with tempfile.TemporaryDirectory(prefix='itt-metal-', dir=output) as tmp:
            src, dst = Path(tmp) / 'source.metal', Path(tmp) / 'output.metallib'
            src.write_bytes(raw)
            r = subprocess.run([compiler, *FLAGS, '-o', str(dst), str(src)],
                               capture_output=True, text=True)
            if r.returncode or not dst.is_file() or not dst.read_bytes().startswith(b'MTLB'):
                return key, None, r.stderr or 'compiler produced no Metal library'
            sha = digest(dst.read_bytes())
            dst.replace(target)
            return key, sha, None

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        rows = list(pool.map(compile_one, sources(cache).items()))
    libraries = {key: sha for key, sha, error in rows if not error}
    errors = {key: error for key, sha, error in rows if error}
    # Package only this manifest's successful libraries, never orphaned files.
    report = dict(compiler=identity, flags=FLAGS, libraries=libraries, errors=errors)
    with tempfile.NamedTemporaryFile(mode='w', dir=output, delete=False) as f:
        json.dump(report, f, indent=2)
        tmp = Path(f.name)
    tmp.replace(manifest)
    return report


def install(output, destination):
    import shutil
    report = json.loads((output / 'itt-libraries.json').read_text())
    destination.mkdir(parents=True, exist_ok=True)
    for key, sha in report['libraries'].items():
        if len(key) != 64 or any(c not in '0123456789abcdef' for c in key):
            raise ValueError('invalid shader source digest')
        src = output / (key + '.metallib')
        if digest(src.read_bytes()) != sha:
            raise ValueError('compiled shader changed: ' + key)
        shutil.copy2(src, destination / src.name)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cache', type=Path, default=Path.home() / 'Library/Application Support/MacNativeGames/ITT/shader-cache' / ('rt-' + CHAIN))
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--install', type=Path)
    args = p.parse_args()
    if args.install:
        if (args.output / 'itt-libraries.json').is_file():
            install(args.output, args.install)
        return
    try:
        compiler = subprocess.check_output(['xcrun', '--find', 'metal'], text=True, stderr=subprocess.DEVNULL).strip()
        identity = subprocess.check_output([compiler, '--version'], text=True, stderr=subprocess.DEVNULL).strip()
    except (OSError, subprocess.CalledProcessError):
        print('ITT: optional Metal compiler unavailable; keep runtime source compilation')
        return
    report = compile_cache(args.cache, args.output, compiler, identity)
    print(f"ITT shader libraries: {len(report['libraries'])} ready, {len(report['errors'])} source fallbacks")


if __name__ == '__main__':
    main()
