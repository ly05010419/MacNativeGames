#!/usr/bin/env python3
"""Compile a translation cache's shaders into Metal libraries ahead of time, for a game binary run from a work folder.

The runtime loads <folder of the game binary>/shader-libs/<SHA-256 of the MSL>.metallib instead of compiling a translated
shader's MSL when a draw first needs it (nvn_mtl.m shader_lib_prebuilt; build_app.py step_shader_libs does the same for
the startup shaders it packs into the app, with the same compiler flags). After `seed-shaders` (scripts/ns2_shader_corpus.py)
has translated every shader a Switch 2 game ships, this compiles those variants. Additional binding states and patched
pipeline variants may still need compilation during play.
Each library is named by its source, so a run reuses every one already built; a different compiler starts over.

  build_shader_libs.py --cache <translation cache (TOTK_SHADER_CACHE)> --out <folder of the game binary>/shader-libs
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO))
from build_app import METAL_FLAGS as BASE_METAL_FLAGS, metal_compiler   # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent / 'dev'))
from game_slot import GameSlot

# Match totk_precise_opts: the invariant position attribute requires this option.
# -fwrapv: SPIR-V, GLSL and the Switch's GPUs wrap on signed overflow; spirv-cross writes signed OpIMul / OpIAdd as plain int
# arithmetic, where overflow is undefined in MSL and Metal's front end optimizes as if it cannot happen. Ocarina's mesh
# shader 35785cddc971290c sign-extends a packed 16-bit light-probe coordinate with `as_type<int>(w) * 65536` and a shift;
# the compiler took the sign from the wrong bits and a wall in the Deku Tree (Gohma's room) was lit brightly. From
# seedds/nxnative docs/reference/shaders.md "Signed integer overflow in translated shaders" (MIT, PR #1844), which measured
# no GPU cost. The runtime's own compiles (nvn_mtl.m totk_precise_opts, a variant no library holds) go through
# MTLCompileOptions, which has no such option: a shader first met in play is compiled without it until the next build.
METAL_FLAGS = [*BASE_METAL_FLAGS, "-fpreserve-invariance", "-fwrapv"]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cache', type=Path, action='append', required=True,
                   help='translation cache; repeat to include recorded binding variants')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--only', type=Path, help='compile only these shaders (one 64-hex key per line); the rest are compiled by the game at first use')
    a = p.parse_args()
    if not 1 <= a.jobs <= 32:
        p.error('--jobs must be between 1 and 32')
    found = metal_compiler()
    if not found:
        sys.exit('[RED] no Metal compiler (Xcode -> Settings -> Components: Metal Toolchain)')
    metal, compiler, env = found
    source_files = {}
    for cache in a.cache:
        for src in sorted(cache.glob('*_b.metal')):
            previous = source_files.get(src.name)
            if previous and digest(previous) != digest(src):
                sys.exit('conflicting translation caches for ' + src.name)
            source_files[src.name] = src
    if a.only:
        wanted = {line.strip() for line in a.only.read_text().splitlines() if line.strip()}
        source_files = {n: s for n, s in source_files.items() if n.split('-')[0].split('_')[0] in wanted}
    sources = list(source_files.values())
    if not sources:
        sys.exit(f'[RED] {a.cache}: no translated shaders (*_b.metal)')
    identity = ' '.join([compiler, *METAL_FLAGS])
    stamp = a.out/'compiler.txt'
    if stamp.is_file() and stamp.read_text() != identity:
        a.out.rename(a.out.with_name(a.out.name + '.previous-' + str(os.getpid())))
    a.out.mkdir(parents=True, exist_ok=True)
    stamp.write_text(identity)
    manifest = a.out/'libraries-manifest.json'
    prior = json.loads(manifest.read_text()) if manifest.exists() else {}
    known = prior.get('files', {}) if prior.get('compiler') == identity else {}
    parser_depths = dict(prior.get('parser_depths', {})) if known else {}
    todo, source_hashes = {}, {}
    # 42,845 sources and as many libraries are hashed on all cores (one thread took minutes per build).
    with concurrent.futures.ThreadPoolExecutor(min(16, (os.cpu_count() or 4) * 2)) as pool:
        source_digests = list(pool.map(digest, sources))
        def lib_ok(sha):
            lib = a.out/f'{sha}.metallib'
            return lib.is_file() and lib.name in known and digest(lib) == known[lib.name]
        unique = sorted(set(source_digests))
        lib_states = dict(zip(unique, pool.map(lib_ok, unique)))
    for src, sha in zip(sources, source_digests):
        source_hashes[src.name] = sha
        if not lib_states[sha]:
            todo.setdefault(sha, src)
    print(f'{len(sources)} translated shaders, {len(todo)} to compile with {compiler}', flush=True)

    def compile_one(item):
        digest, src = item
        partial = a.out/f'{digest}.partial'
        r = subprocess.run([metal, *METAL_FLAGS, '-o', partial, src], capture_output=True, text=True, env=env)
        parser_depth = None
        if r.returncode and 'bracket nesting level exceeded maximum of 256' in r.stderr:
            # Two genuine OOT shaders have deeper structured control flow. This
            # changes only the compiler's parser limit, not their code or math.
            partial.unlink(missing_ok=True)
            parser_depth = 1024
            r = subprocess.run([metal, *METAL_FLAGS, '-fbracket-depth=1024', '-o', partial, src],
                               capture_output=True, text=True, env=env)
        if r.returncode:
            partial.unlink(missing_ok=True)
            (a.out/f'{digest}.metallib').unlink(missing_ok=True)
            return digest, parser_depth, f'{src.name}\n{r.stdout}{r.stderr}'
        partial.replace(a.out/f'{digest}.metallib')
        return digest, parser_depth, None

    failed, done = [], 0
    os.environ.setdefault('TOTK_GAME_SLOT_DIR', str(Path.home() / 'Library/Application Support/MacNativeGames/TOTK/game-slot'))
    with GameSlot(owner='build', purpose='OOT offline Metal libraries', expect_seconds=1800):
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
            for source_hash, parser_depth, f in pool.map(compile_one, todo.items()):
                done += 1
                if parser_depth:
                    parser_depths[source_hash + '.metallib'] = parser_depth
                if f:
                    failed.append(f)
                if done % 250 == 0:
                    print(f'  {done} / {len(todo)}', flush=True)
    log = a.out/'failed.log'
    log.write_text('\n'.join(failed))
    print(f'{len(todo) - len(failed)} compiled, {len(failed)} failed' + (f' (incomplete; {log})' if failed else ''))
    # Preserve verified successes even when another shader failed; a retry only
    # compiles the missing/changed outputs. Incomplete manifests cannot be packed.
    files = {sha + '.metallib': digest(a.out/(sha + '.metallib')) for sha in sorted(set(source_hashes.values()))
             if (a.out/(sha + '.metallib')).is_file()}
    # Keep previously verified binding variants too. Runtime lookup requires the
    # exact MSL hash, so unrelated libraries cannot be selected by a shader.
    for name, expected in known.items():
        if name in files or Path(name).name != name or not name.endswith('.metallib'):
            continue
        library = a.out/name
        if library.is_file() and digest(library) == expected:
            files[name] = expected
    report = dict(schema=1, compiler=identity, complete=not failed, subset=bool(a.only), sources=source_hashes, files=files,
                  parser_depths={k: v for k, v in parser_depths.items() if k in files},
                  preparation_sha256=digest(Path(__file__).resolve()))
    partial = manifest.with_suffix('.partial')
    partial.write_text(json.dumps(report, indent=2) + '\n')
    partial.replace(manifest)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
