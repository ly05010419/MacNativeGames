#!/usr/bin/env python3
"""Compile the startup shaders of the seed into Metal libraries ahead of time (build_app.py step_shader_libs, step 3/4 of
the Tears of the Kingdom app build).

The seed (scripts/prepare_shader_seed.py, `make seed`) holds the translated shaders as Metal source; without this step
Metal compiles each one the first time the game draws with it, which is what keeps the screen black when a new area
loads (2026-10-22: 37.5 s in one frame for 157 pipelines and 100 libraries). The runtime looks for
<work>/build/shader-libs/<SHA-256 of the source>.metallib next to its executable (nvn_mtl.m shader_lib_prebuilt), so only
an exact match is used and a rebuild reuses every library whose source did not change. The libraries are game-derived
data and stay in the work folder.

Super Smash Bros. Ultimate: the seed holds every shader of data.arc since 2026-10-24 (Q55; before, it had none), and the
sources are that seed (--seed) and the runtime's own
translation cache, <work>/build/shader-cache/rt-<chain> (--cache), where the game keeps the MSL of every shader it has
translated (<key>_b.metal, the files its warm-up loads) and of every patched variant it compiled (variants/<SHA-256>.metal,
nvn_mtl.m variant_record). The runtime hashes exactly these files, so the libraries match them byte for byte; a shader
translated later is compiled by the game until the next run of this script.
Measured on Smash (M1 Max, 2026-10-23, Q44 title runs: builder b440-b449, check c445-c449, rework r440-r449; log lines
管线预热完成 and [长帧] 帧 3). Metal keeps what the game compiles from source under the length of the folder path the
program runs from (build_app.py step_warm_metal): without these files the first start in a folder of a length Metal had
not compiled these sources for warmed up for 34.1-39.5 s (r440 and r443 with a binary already warm at another length,
c446 with a new one), the second start there in 0.94 s (r441). With these files the first start at a new length took
0.92-1.02 s in 4 of 5 runs (c445, r442, r448, r449); in r444 the warm-up had not finished after about 45 s and the next
start there still took 4.5 s, 3.9 s of it pipelines (r445), cause unmeasured. In a folder Metal knows, a start takes the
same with or without them (warm-up 0.78-1.12 s with, 0.79-1.11 s without). The first start that ever loaded these files
(b443, 1-min load 14) had not finished after about 45 s, the next took 5.3 s, 4.7 s of it pipelines (b444); a later
match run with the same bytes (b449) took 48.9 s, 47.2 s of it pipelines, cause unmeasured. The files depend on the
source and the output file name only, not on the folder (two builds in two folders, diff -rq clean), so a rebuild gives
the same bytes; files compiled any other way (another output name) differ.

    build_shader_libs.py --seed <work>/build/shader-seed --out <work>/build/shader-libs [--jobs N]
    build_shader_libs.py --cache <work>/build/shader-cache/rt-<chain> --out <work>/build/shader-libs [--jobs N]
"""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

# The options of totk_precise_opts() in nvn_mtl.m (as build_app.py): the translated shaders need exactly rounded floats.
METAL_FLAGS = ['-fmetal-math-mode=safe']


def developer_dirs():
    """Developer folders that may hold the Metal toolchain: DEVELOPER_DIR, the one xcode-select points at, then the
    Xcodes in /Applications (build_app.py developer_dirs, without the search on other volumes)."""
    found = [os.environ['DEVELOPER_DIR']] if os.environ.get('DEVELOPER_DIR') else []
    try:
        found.append(subprocess.check_output(['xcode-select', '-p'], text=True, stderr=subprocess.DEVNULL).strip())
    except (OSError, subprocess.CalledProcessError):
        pass
    found += [str(p/'Contents/Developer') for p in sorted(Path('/Applications').glob('Xcode*.app'))]
    return list(dict.fromkeys(found))


def metal_compiler():
    """(compiler, its version, the environment to run it with), or None (build_app.py metal_compiler)."""
    for developer in developer_dirs():
        env = dict(os.environ, DEVELOPER_DIR=developer)
        found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True, env=env)
        metal = found.stdout.strip()
        if found.returncode or not metal:
            continue
        version = subprocess.run([metal, '--version'], capture_output=True, text=True, env=env)
        if not version.returncode and version.stdout:
            return metal, version.stdout.splitlines()[0], env
    return None


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--seed', type=Path, help='the prepared seed: <work>/build/shader-seed')
    p.add_argument('--cache', type=Path, action='append', default=[],
                   help='a runtime translation cache <work>/build/shader-cache/rt-<chain>: its <key>_b.metal and '
                        'variants/*.metal (repeatable)')
    p.add_argument('--out', type=Path, required=True, help='<work>/build/shader-libs (next to the runtime executable)')
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = p.parse_args()
    if not a.seed and not a.cache:
        p.error('give --seed or --cache')
    sources = sorted(a.seed.glob('*.metal')) if a.seed else []
    for cache in a.cache:
        found = sorted(cache.glob('*_b.metal')) + sorted((cache/'variants').glob('*.metal'))
        print(f'{cache}: {len(found)} Metal sources', flush=True)
        sources += found
    if not sources:
        sys.exit(f'no Metal sources in {a.seed or ", ".join(map(str, a.cache))}: run make seed first, or play once '
                 f'so the game fills its translation cache')
    found = metal_compiler()
    if not found:
        sys.exit('no Metal toolchain: xcodebuild -downloadComponent MetalToolchain')
    metal, compiler, env = found
    out = a.out
    stamp = out/'compiler.txt'
    identity = ' '.join([compiler, *METAL_FLAGS])
    if stamp.is_file() and stamp.read_text() != identity:
        shutil.rmtree(out)   # libraries of another compiler or other options: never mixed with these
    out.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)
    todo, digests = {}, set()
    for src in sources:
        digest = hashlib.sha256(src.read_bytes()).hexdigest()
        digests.add(digest)
        if not (out/f'{digest}.metallib').is_file():
            todo.setdefault(digest, src)
    # distinct: sources with the same text share one library (Smash 2026-10-23: 2,889 sources, 2,453 distinct)
    print(f'{len(sources)} shaders ({len(digests)} distinct sources), {len(digests) - len(todo)} already compiled, '
          f'compiling {len(todo)} with {compiler} on {a.jobs} jobs', flush=True)

    def compile_one(item):
        digest, src = item
        partial = out/f'{digest}.partial'
        r = subprocess.run([metal, *METAL_FLAGS, '-o', partial, src], capture_output=True, text=True, env=env)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src.name}\n{r.stdout}{r.stderr}'
        partial.replace(out/f'{digest}.metallib')
        return None

    t0, done, failed = time.time(), 0, []
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for f in pool.map(compile_one, todo.items()):
            done += 1
            if f:
                failed.append(f)
            if done % 500 == 0 or done == len(todo):
                print(f'compiled {done}/{len(todo)}, {len(failed)} failed; {time.time() - t0:.0f} s', flush=True)
    (out/'failed.log').write_text('\n'.join(failed))
    have = len(list(out.glob('*.metallib')))
    print(f'shader libraries ready: {have} in {out}; {len(failed)} did not compile (the game compiles those at launch, '
          f'{out/"failed.log"})')
    return 1 if have == 0 else 0


if __name__ == '__main__':
    sys.exit(main())
