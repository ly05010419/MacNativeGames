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


# libraries-verified.json: each library's device, inode, size and modification time with the digest it was checked with.
# A library still that file keeps the digest instead of being read again (user 2026-11-11 "极速更新": reading 42,891
# sources and every library took 2 minutes of each update that compiled none). The change time is not compared: packing
# links the libraries into the app's shader data bundle, which changes their inodes'.
def lib_identity(path):
    s = path.stat()
    return [s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns]


def seed_record(seed):
    """name -> SHA-256 of the seed's files that are still the files its .verified.json recorded (prepare_ns2_seed.py,
    record_verified): same device, inode, size and modification time. Not the change time: the old seed, removed in the
    background after a publish, shares these files' inodes, and each link it drops moves their change time."""
    try:
        record = json.loads(seed.with_name(seed.name + '.verified.json').read_text())
        files = record['files'] if record.get('schema') == 1 else {}
    except (OSError, ValueError, KeyError):
        return {}
    out = {}
    for name, entry in files.items():
        if not name.endswith('_b.metal') or not isinstance(entry, list) or len(entry) != 6:
            continue
        try:
            if entry[:4] == lib_identity(seed / name):
                out[name] = entry[5]
        except OSError:
            pass
    return out


def recorded_libraries(out):
    try:
        record = json.loads((out/'libraries-verified.json').read_text())
        return record['files'] if record.get('schema') == 1 and isinstance(record.get('files'), dict) else {}
    except (OSError, ValueError, KeyError):
        return {}


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
    # A source the seed recorded as checked (prepare_ns2_seed.py record_verified) keeps that digest; the rest, and every
    # library not recorded as unchanged, are hashed on all cores (one thread took minutes per build).
    seed_known = {}
    for cache in a.cache:
        seed_known.update(seed_record(cache))
    lib_record = recorded_libraries(a.out)
    read = [0, 0]
    with concurrent.futures.ThreadPoolExecutor(min(16, (os.cpu_count() or 4) * 2)) as pool:
        def source_digest(src):
            if seed_known.get(src.name) and src.parent.resolve() in {c.resolve() for c in a.cache}:
                return seed_known[src.name]
            read[0] += 1
            return digest(src)
        source_digests = list(pool.map(source_digest, sources))
        def lib_ok(sha):
            lib = a.out/f'{sha}.metallib'
            if not (lib.is_file() and lib.name in known):
                return False
            entry = lib_record.get(lib.name)
            try:
                if isinstance(entry, list) and len(entry) == 5 and entry[:4] == lib_identity(lib) and entry[4] == known[lib.name]:
                    return True
            except OSError:
                return False
            read[1] += 1
            return digest(lib) == known[lib.name]
        unique = sorted(set(source_digests))
        lib_states = dict(zip(unique, pool.map(lib_ok, unique)))
    for src, sha in zip(sources, source_digests):
        source_hashes[src.name] = sha
        if not lib_states[sha]:
            todo.setdefault(sha, src)
    print(f'{len(sources)} translated shaders, {len(todo)} to compile with {compiler} '
          f'({read[0]} sources and {read[1]} libraries read; the rest kept the digests recorded with them)', flush=True)

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
    record = dict(schema=1, files={name: lib_identity(a.out/name) + [sha] for name, sha in sorted(files.items())})
    (a.out/'libraries-verified.json.tmp').write_text(json.dumps(record, separators=(',', ':')))
    (a.out/'libraries-verified.json.tmp').replace(a.out/'libraries-verified.json')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
