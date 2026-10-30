#!/usr/bin/env python3
"""Compile the game's shaders ahead of time into <work>/build/shader-libs/<SHA-256 of the MSL>.metallib.

The runtime (nvn_mtl.m totk_msl_library) loads a library from there when one matches the MSL it is about to compile, and
keeps the MSL of every library it still compiles at runtime in shader-libs/src. Sources compiled here: every translated
shader in the runtime translation cache, plus those recorded variants. macOS's own cache of runtime compiles is tied to
the executable and starts empty after each rebuild; these files are not. Run it after playing to add what was new.

Options match the runtime's (totk_precise_opts): safe math, and invariant positions (DM_PRESERVE_INVARIANCE, which the
Echoes of Wisdom runtime sets). flags.txt records them; the runtime ignores the folder if its own options differ.
usage: build_shader_libs.py [--work EoW_build] [--jobs N]
"""
import argparse, concurrent.futures, hashlib, os, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
FLAGS = ['-fmetal-math-mode=safe', '-fpreserve-invariance']


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--work', type=Path, default=HERE / 'EoW_build')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    build = args.work.resolve() / 'build'
    caches = sorted(build.glob('shader-cache/rt-*'))
    libs = build / 'shader-libs'
    found = subprocess.run(['xcrun', '--find', 'metal'], capture_output=True, text=True)
    if found.returncode or not found.stdout.strip():
        sys.exit('no Metal compiler: install the Metal toolchain (xcodebuild -downloadComponent MetalToolchain)')
    metal = found.stdout.strip()
    version = subprocess.run([metal, '--version'], capture_output=True, text=True).stdout.splitlines()[0]
    libs.mkdir(parents=True, exist_ok=True)
    identity = f'{version} {" ".join(FLAGS)}'
    stamp = libs / 'compiler.txt'
    if stamp.is_file() and stamp.read_text() != identity:   # another compiler or other options: start over
        for old in libs.glob('*.metallib'):
            old.unlink()
    stamp.write_text(identity)
    (libs / 'flags.txt').write_text(' '.join(FLAGS) + '\n')

    todo = {}
    sources = [p for c in caches for p in c.glob('*.metal')] + sorted((libs / 'src').glob('*.metal'))
    for src in sources:
        digest = hashlib.sha256(src.read_bytes()).hexdigest()
        if not (libs / f'{digest}.metallib').is_file():
            todo.setdefault(digest, src)
    print(f'{len(sources)} shader sources, {len(todo)} to compile ({len(sources) - len(todo)} already built)', flush=True)

    def compile_one(item):
        digest, src = item
        partial = libs / f'{digest}.partial'
        r = subprocess.run([metal, *FLAGS, '-o', str(partial), str(src)], capture_output=True, text=True)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src}\n{r.stdout}{r.stderr}'
        partial.replace(libs / f'{digest}.metallib')
        return None

    failed, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for f in pool.map(compile_one, todo.items()):
            done += 1
            if f: failed.append(f)
            if done % 100 == 0: print(f'  {done} / {len(todo)}', flush=True)
    (libs / 'failed.log').write_text('\n'.join(failed))
    built = len(list(libs.glob('*.metallib')))
    print(f'done: {built} libraries in {libs}; {len(failed)} failed (compiled at runtime instead, see failed.log)')


if __name__ == '__main__':
    main()
