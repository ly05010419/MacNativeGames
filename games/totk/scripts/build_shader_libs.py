#!/usr/bin/env python3
"""Compile a translation cache's shaders into Metal libraries ahead of time, for a game binary run from a work folder.

The runtime loads <folder of the game binary>/shader-libs/<SHA-256 of the MSL>.metallib instead of compiling a translated
shader's MSL when a draw first needs it (nvn_mtl.m shader_lib_prebuilt; build_app.py step_shader_libs does the same for
the startup shaders it packs into the app, with the same compiler flags). After `seed-shaders` (scripts/ns2_shader_corpus.py)
has translated every shader a Switch 2 game ships, this compiles all of them, so none is compiled while playing.
Each library is named by its source, so a run reuses every one already built; a different compiler starts over.

  build_shader_libs.py --cache <translation cache (TOTK_SHADER_CACHE)> --out <folder of the game binary>/shader-libs
"""
import argparse
import concurrent.futures
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
from build_app import METAL_FLAGS, metal_compiler   # noqa: E402


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = p.parse_args()
    found = metal_compiler()
    if not found:
        sys.exit('[RED] no Metal compiler (Xcode -> Settings -> Components: Metal Toolchain)')
    metal, compiler, env = found
    sources = sorted(a.cache.glob('*_b.metal'))
    if not sources:
        sys.exit(f'[RED] {a.cache}: no translated shaders (*_b.metal)')
    identity = ' '.join([compiler, *METAL_FLAGS])
    stamp = a.out/'compiler.txt'
    if stamp.is_file() and stamp.read_text() != identity:
        shutil.rmtree(a.out)
    a.out.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)
    todo = {}
    for src in sources:
        digest = hashlib.sha256(src.read_bytes()).hexdigest()
        if not (a.out/f'{digest}.metallib').is_file():
            todo.setdefault(digest, src)
    print(f'{len(sources)} translated shaders, {len(todo)} to compile with {compiler}', flush=True)

    def compile_one(item):
        digest, src = item
        partial = a.out/f'{digest}.partial'
        r = subprocess.run([metal, *METAL_FLAGS, '-o', partial, src], capture_output=True, text=True, env=env)
        if r.returncode:
            partial.unlink(missing_ok=True)
            return f'{src.name}\n{r.stdout}{r.stderr}'
        partial.replace(a.out/f'{digest}.metallib')
        return None

    failed, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        for f in pool.map(compile_one, todo.items()):
            done += 1
            if f:
                failed.append(f)
            if done % 250 == 0:
                print(f'  {done} / {len(todo)}', flush=True)
    log = a.out/'failed.log'
    log.write_text('\n'.join(failed))
    print(f'{len(todo) - len(failed)} compiled, {len(failed)} failed' + (f' (they compile at launch; {log})' if failed else ''))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
