#!/usr/bin/env python3
"""Compile the game's shaders ahead of time into <work>/build/shader-libs/<SHA-256 of the MSL>.metallib.

Forked from games/eow. The runtime (nvn_mtl.m shader_lib_prebuilt) loads a library from there when one matches the MSL
it is about to compile. macOS's own cache of runtime compiles is tied to the executable and starts empty after each
rebuild; these files are not. Dread's copy of the runtime does not keep the MSL it compiles (no shader-libs/src).

  build_shader_libs.py --seed     every shader of the published seed (<work>/build/shader-seed), compiled beside the
                                  published folder (shader-libs.partial) and swapped in only when every MSL of the
                                  seed has its library; any failure exits non-zero and leaves the published folder as
                                  it was. Libraries already built with the same compiler and options, and
                                  shader-libs/src, are kept. seed.txt names the seed manifest built for.
                                  build.py runs it right after prepare_shader_seed.py and stops on a failure, so an
                                  app is not built with a seed its libraries do not serve.
  build_shader_libs.py --check    whether the published folder serves the seed: options, compiler, seed.txt, and a
                                  library for every MSL of the seed (exit 1 and the reasons otherwise).
  build_shader_libs.py            after playing: the translation cache's shaders and shader-libs/src not built yet,
                                  added in place (a failure is compiled at runtime instead, see failed.log).

Options match the runtime's (totk_precise_opts): safe math (Metroid Dread's launcher does not set
DM_PRESERVE_INVARIANCE). flags.txt records them for --check; Dread's runtime does not read it.
usage: build_shader_libs.py [--work Dread_build] [--jobs N] [--seed | --check]
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parents[1]
FLAGS = ['-fmetal-math-mode=safe']
RENAME_SWAP = 2   # <stdio.h> renamex_np: exchange the two paths in one step (APFS)


class LibsError(Exception):
    pass


def exchange(staging, target):
    """Publish the complete folder staging as target in one step: an exchange of the two names (renamex_np
    RENAME_SWAP), so target is always either the old folder or the new one, never missing. The old folder is then at
    staging, kept for the caller to remove or to exchange back; with nothing published yet, a plain rename."""
    if not target.exists():
        staging.rename(target)
        return
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.renamex_np(os.fsencode(staging), os.fsencode(target), RENAME_SWAP):
        raise OSError(ctypes.get_errno(), f'cannot exchange {staging} and {target}')


def swap_in(staging, target):
    """exchange, then remove the old folder."""
    exchange(staging, target)
    shutil.rmtree(staging, ignore_errors=True)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def compiler(metal=None, required=True):
    """The Metal compiler and its identity (version line and options), as compiler.txt records it; (None, None) when
    there is none and it is not required."""
    if not metal:
        found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True)
        if found.returncode or not found.stdout.strip():
            if not required:
                return None, None
            raise LibsError('no Metal compiler: install the Metal toolchain (xcodebuild -downloadComponent '
                            'MetalToolchain) or set DEVELOPER_DIR to Xcode')
        metal = found.stdout.strip()
    version = subprocess.run([metal, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    return metal, f'{version} {" ".join(FLAGS)}'


def compile_all(metal, todo, libs, jobs):
    """{digest: source} -> libs/<digest>.metallib; returns the failures (source and the compiler's output)."""
    def one(item):
        d, src = item
        partial = libs / f'{d}.partial'
        r = subprocess.run([metal, *FLAGS, '-o', str(partial), str(src)], capture_output=True, text=True)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src}\n{r.stdout}{r.stderr}'
        partial.replace(libs / f'{d}.metallib')
        return None
    failed, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for f in pool.map(one, todo.items()):
            done += 1
            if f:
                failed.append(f)
            if done % 1000 == 0:
                print(f'  {done} / {len(todo)}', flush=True)
    return failed


def seed_sources(seed):
    """{SHA-256 of the MSL: source} of a seed folder, each source checked against the seed's manifest."""
    if not (seed / 'seed-manifest.json').is_file():
        raise LibsError(f'no prepared seed in {seed} (make shader-seed)')
    try:
        listed = {n: h for n, h in json.loads((seed / 'seed-manifest.json').read_text())['files'].items()
                  if n.endswith('_b.metal')}
    except (ValueError, KeyError, TypeError, AttributeError):
        raise LibsError(f'{seed}/seed-manifest.json cannot be read: prepare the seed again') from None
    sources = {}
    for src in sorted(seed.glob('*_b.metal')):
        d = digest(src)
        if listed.pop(src.name, None) != d:
            raise LibsError(f'{src} is not the file its seed manifest names: prepare the seed again')
        sources.setdefault(d, src)
    if listed:
        raise LibsError(f'{len(listed)} shader sources the seed manifest names are missing: prepare the seed again')
    return sources


def stage_seed_libs(libs, seed, jobs, metal=None):
    """Every MSL of the seed folder compiled into <libs>.partial, which is returned complete; the published libs folder
    is not touched. Any failure removes the partial folder and raises LibsError."""
    sources = seed_sources(seed)
    metal, identity = compiler(metal)
    staging = libs.with_name(libs.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    kept = 0
    if (libs / 'compiler.txt').is_file() and (libs / 'compiler.txt').read_text() == identity:
        for old in libs.glob('*.metallib'):   # same compiler and options: the same bytes, linked instead of rebuilt
            os.link(old, staging / old.name)
            kept += 1
    if (libs / 'src').is_dir():
        shutil.copytree(libs / 'src', staging / 'src')
    todo = {d: src for d, src in sources.items() if not (staging / f'{d}.metallib').is_file()}
    print(f'{len(sources)} seed shader sources, {len(todo)} to compile ({kept} libraries kept)', flush=True)
    failed = compile_all(metal, todo, staging, jobs)
    missing = [d for d in sources if not (staging / f'{d}.metallib').is_file()]
    if failed or missing:
        log = libs.with_name('shader-libs-failed.log')
        log.write_text('\n'.join(failed))
        shutil.rmtree(staging)
        raise LibsError(f'{len(failed)} failed, {len(missing)} missing (see {log})')
    (staging / 'compiler.txt').write_text(identity)
    (staging / 'seed.txt').write_text(f'{digest(seed / "seed-manifest.json")} {len(sources)}\n')
    (staging / 'flags.txt').write_text(' '.join(FLAGS) + '\n')
    return staging, len(sources)


def build_seed(build, jobs, metal):
    libs = build / 'shader-libs'
    try:
        staging, count = stage_seed_libs(libs, build / 'shader-seed', jobs, metal)
    except LibsError as error:
        raise LibsError(f'{error}; {libs} is unchanged') from None
    swap_in(staging, libs)
    print(f'done: every one of the seed\'s {count} shader sources has its library in {libs}')


def check(build, metal):
    seed = build / 'shader-seed'
    sources = seed_sources(seed)
    _, identity = compiler(metal, required=False)
    libs, problems = build / 'shader-libs', []

    def text(name):
        return (libs / name).read_text() if (libs / name).is_file() else None
    if text('flags.txt') != ' '.join(FLAGS) + '\n':
        problems.append('flags.txt is missing or names other options than the runtime\'s')
    if identity is None:
        print('shader libraries: compiler not compared (no Metal compiler found)')
    elif text('compiler.txt') != identity:
        problems.append('built with another compiler or options')
    if text('seed.txt') != f'{digest(seed / "seed-manifest.json")} {len(sources)}\n':
        problems.append('built for another seed')
    missing = [d for d in sources if not (libs / f'{d}.metallib').is_file()]
    if missing:
        problems.append(f'{len(missing)} of the seed\'s {len(sources)} shader sources have no library')
    for p in problems:
        print(f'shader libraries: {p}')
    if problems:
        sys.exit(1)
    print(f'shader libraries serve the seed: {len(sources)} sources, each with its library')


def build_played(build, jobs, metal):
    metal, identity = compiler(metal)
    libs = build / 'shader-libs'
    libs.mkdir(parents=True, exist_ok=True)
    stamp = libs / 'compiler.txt'
    if stamp.is_file() and stamp.read_text() != identity:   # another compiler or other options: start over
        for old in libs.glob('*.metallib'):
            old.unlink()
    stamp.write_text(identity)
    (libs / 'flags.txt').write_text(' '.join(FLAGS) + '\n')
    todo = {}
    sources = [p for c in sorted(build.glob('shader-cache/rt-*')) for p in c.glob('*.metal')] + \
        sorted((libs / 'src').glob('*.metal'))
    for src in sources:
        d = digest(src)
        if not (libs / f'{d}.metallib').is_file():
            todo.setdefault(d, src)
    print(f'{len(sources)} shader sources, {len(todo)} to compile ({len(sources) - len(todo)} already built)',
          flush=True)
    failed = compile_all(metal, todo, libs, jobs)
    (libs / 'failed.log').write_text('\n'.join(failed))
    built = len(list(libs.glob('*.metallib')))
    print(f'done: {built} libraries in {libs}; {len(failed)} failed (compiled at runtime instead, see failed.log)')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, default=HERE.parent / 'Dread_build')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--metal', help=argparse.SUPPRESS)   # tests: a stand-in compiler
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument('--seed', action='store_true', help='compile the prepared seed, all or nothing')
    mode.add_argument('--check', action='store_true', help='whether the libraries serve the prepared seed')
    args = ap.parse_args(argv)
    build = args.work.resolve() / 'build'
    try:
        if args.seed:
            build_seed(build, args.jobs, args.metal)
        elif args.check:
            check(build, args.metal)
        else:
            build_played(build, args.jobs, args.metal)
    except LibsError as error:
        sys.exit(f'shader libraries: {error}')


if __name__ == '__main__':
    main()
