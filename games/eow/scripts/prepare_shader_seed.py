#!/usr/bin/env python3
"""Translate Echoes of Wisdom's shaders from your own game files ahead of play, so play does not stop to translate them.

    prepare_shader_seed.py --work EoW_build [--jobs N]          # <work>/build/shader-seed
    prepare_shader_seed.py --work EoW_build --list-from <played shader cache>...

Forked from games/dread/scripts/prepare_shader_seed.py (the same runtime family: translation chain
totk-native-v4-unverified, third_party/d2r_gfx/gfx/nvn/dmsh_chain.h) for this game's containers. Every shader of
1.1.0 sits in a complete BNSH file: inside the BFSHA archives and the effect files (.ptcl) under region_common/, as
.bnsh members of the UI layout archives (.arc, SARC), and embedded in the main module (rewritten/v110/main.patched.bin).
The RomFS holds no compressed container (a census of 1.1.0 on 2026-11-12: 9,858 unique shaders, 5,268 vertex, 4,553
pixel, 37 compute; EXPECTED below). Each shader's key is the runtime's (nvn_mtl.m prog_get: SHA-256 of the 0x880-byte
control section, then the code).

Each shader is translated for the default bind state, then for every bind state the V rows of assets/shader_list.tsv
name for it, as the runtime would ask (totk_shader_rt.m totk_rt_variant: the variant is the key plus the first 12 hex
digits of the SHA-256 of the answers as compact sorted JSON; deps_match picks a variant, defaults 2d / float). Clip
depth is not converted (depth_minus_w false: the game never selects NVN_DEPTH_MODE_NEAR_IS_MINUS_W, and a seed
translated so had byte-identical MSL to the runtime's own translations, so the libraries build_shader_libs.py --seed
compiles are the ones the runtime looks up).

A failed translation stops the run, except the keys in EXCLUDED, each with the reason; their result is recorded in
seed-manifest.json (excluded). The seed and its Metal libraries are published as a pair: the seed is written beside the
old one (<output>.partial), every MSL of it is compiled beside the old libraries (build_shader_libs.py stage_seed_libs:
<work>/build/shader-libs.partial), and only when both are complete are they published (publish): the libraries, then
the seed, each with one exchange of the two folder names (build_shader_libs.py exchange). The old pair stays in the
.partial folders until both exchanges are done; a failure or an interrupt at either puts the old pair back. A failed
translation or compile leaves both as they were. A process killed between the two exchanges leaves the new libraries
with the old seed (the old libraries in shader-libs.partial until the next run): with the same compiler the new
libraries hold every old one, so the old seed is fully served; after a compiler change, the old seed's shaders they
lack are compiled in play, and build_shader_libs.py --check (make boot) says so until make shader-seed runs again.
The seed holds game-derived data: the work folder only, never the repo.

--list-from adds to assets/shader_list.tsv each bind state a played shader cache met that no seed variant serves.
"""
import argparse
import concurrent.futures
import hashlib
import importlib.util
import inspect
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shader_list   # noqa: E402
from shader_list import ANSWERS, CHAIN, HEADER, answers_for, canonical, deps_match, variant_name   # noqa: E402,F401

VERSION = 'v110'
EXPECTED = dict(shaders=9858, vs=5268, ps=4553, cs=37)   # 1.1.0's census; anything else: another game or a bad read
LIST = Path(__file__).resolve().parents[1] / 'assets/shader_list.tsv'
DEPTH_MINUS_W = False
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}
COMPRESSED = (b'\x28\xb5\x2f\xfd', b'Yaz0')
# Shaders the translator cannot translate yet: the run goes on without them and records the error. Exact keys only.
EXCLUDED = {
    '06d3097fd20d6199f988dfcf1ed234bf82d72e7fa1f4a2d68267d061a42a40c5':
        'region_common/shader/postprocess.bfsha pixel shader: SPIRV-Cross rejects the translator\'s SPIR-V (nullptr); '
        'never requested in recorded play',
    '3281c991e8ffd0a7b81c48e63a2c019686f88a6b3f5cb2a111d20b6862c93063':
        'region_common/shader/postprocess.bfsha pixel shader: SPIRV-Cross rejects the translator\'s SPIR-V (nullptr); '
        'never requested in recorded play',
}


class SeedError(Exception):
    pass


_LIBRARIES = []


def libraries_module():
    """scripts/build_shader_libs.py (stage_seed_libs, exchange, swap_in, LibsError), loaded once."""
    if not _LIBRARIES:
        path = Path(__file__).with_name('build_shader_libs.py')
        spec = importlib.util.spec_from_file_location('eow_build_shader_libs', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _LIBRARIES.append(module)
    return _LIBRARIES[0]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def checked(data, offset, size):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise SeedError('shader container range outside file')
    return data[offset:offset + size]


def sarc(data):
    """The members of a SARC archive (the UI layout .arc files) -> {name: bytes}."""
    endian = '<' if data[6:8] == b'\xff\xfe' else '>'
    header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
    if size != len(data) or header < 20 or checked(data, header, 4) != b'SFAT':
        raise SeedError('invalid SARC header')
    count = struct.unpack_from(endian + 'H', data, header + 6)[0]
    names = header + 12 + count * 16
    if checked(data, names, 4) != b'SFNT':
        raise SeedError('invalid SARC string table')
    names += 8
    result = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from(endian + '4I', checked(data, header + 12 + i * 16, 16))
        off = names + (attribute & 0xffffff) * 4
        if attribute >> 24 != 1 or not names <= off < start or hi < lo:
            raise SeedError('invalid SARC member')
        result[data[off:data.index(0, off, start)].decode('utf-8')] = checked(data, start + lo, hi - lo)
    return result


def bnsh_shaders(data):
    """One BNSH file -> (key, stage, control, code). Offsets are relative to the BNSH file, not its container."""
    if data[:4] != b'BNSH' or checked(data, 12, 2) != b'\xff\xfe' or checked(data, 96, 4) != b'grsc':
        raise SeedError('unsupported BNSH header')
    data = checked(data, 0, struct.unpack_from('<I', data, 28)[0])
    count, variations = struct.unpack_from('<IQ', data, 124)
    checked(data, variations, count * 64)
    for i in range(count):
        program = struct.unpack_from('<Q', data, variations + i * 64 + 16)[0]
        checked(data, program, 56)
        for stage, relative in (('vs', 8), ('ps', 40), ('cs', 48)):
            offset = struct.unpack_from('<Q', data, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, code_size, control_size = struct.unpack_from('<QQII', checked(data, offset + 8, 24))
            if control_size < 0x880:   # the SDK's older, shorter layout: not a section the game binds
                continue
            control, code = checked(data, control_off, 0x880), checked(data, code_off, code_size)
            c1, length = struct.unpack_from('<II', control, 0x700)
            if STAGES.get(struct.unpack_from('<I', control, 0x714)[0]) != stage or not 0 <= c1 <= length <= len(code) \
                    or not length:
                raise SeedError('invalid NVN shader bounds or stage')
            yield digest(control + code[:length]), stage, control, code[:length]


def inventory(romfs, modules):
    """Every shader of the RomFS and the main module -> {key: (stage, control, code, source)}, and the files read."""
    index = json.loads((romfs / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise SeedError('the RomFS has not passed integrity verification (scripts/extract_romfs.py)')
    shaders, archives = {}, []

    def visit(name, data, depth=0):
        if data[:4] == b'SARC' and depth < 4:
            for member, raw in sarc(data).items():
                visit(name + '/' + member, raw, depth + 1)
            return
        pos = 0
        while (pos := data.find(b'BNSH\0\0\0\0', pos)) >= 0:
            size = struct.unpack_from('<I', checked(data, pos + 28, 4))[0]
            for key, stage, control, code in bnsh_shaders(checked(data, pos, size)):
                shaders.setdefault(key, (stage, control, code, name))
            pos += max(size, 8)

    with (romfs / 'romfs.bin').open('rb') as stream:
        for row in sorted(index['files'], key=lambda r: r['path']):
            stream.seek(row['offset'])
            raw = stream.read(row['size'])
            if len(raw) != row['size']:
                raise SeedError('short RomFS read')
            if raw[:4] in COMPRESSED:
                raise SeedError(f'{row["path"]} is compressed: 1.1.0 has no such file, this script cannot read it')
            if raw[:4] == b'SARC' or b'BNSH\0\0\0\0' in raw:
                before = len(shaders)
                visit(row['path'], raw)
                archives.append(dict(path=row['path'], sha256=digest(raw), new_shaders=len(shaders) - before))
    module = modules / 'main.patched.bin'
    if not module.is_file():
        raise SeedError(f'{module} is missing (the rewrite)')
    visit('module:main', module.read_bytes())
    return shaders, archives


def census(shaders):
    """The inventory against EXPECTED: a different count means another game version or a broken read."""
    found = dict(shaders=len(shaders), **{s: sum(1 for v in shaders.values() if v[0] == s) for s in ('vs', 'ps', 'cs')})
    if found != EXPECTED:
        raise SeedError(f'shader census {found}, expected {EXPECTED} (Echoes of Wisdom 1.1.0)')
    return found


def read_list(path):
    """The list's rows (shader key, deps): shader_list.py read."""
    try:
        return shader_list.read(path)
    except shader_list.ListError as error:
        raise SeedError(str(error)) from None


def plan(shaders, listed):
    """Jobs (key, answers, variant, why): the default bind state of every shader, then each listed one of this game."""
    jobs = [(key, ANSWERS, variant_name(key, ANSWERS), 'default') for key in sorted(shaders)]
    for key, deps in sorted(listed, key=lambda r: (r[0], canonical(r[1]))):
        if key in shaders:
            answers = answers_for(deps)
            jobs.append((key, answers, variant_name(key, answers), 'listed'))
    return jobs


def translation_request(variant, stage, control, code, answers, code_path):
    """What the translator is asked, as totk_shader_rt.m totk_rt_variant asks it (graphics code without its 0x30-byte
    NVN header). A cached translation depends on this function's source (TranslationCache identity)."""
    c1, length = struct.unpack_from('<II', control, 0x700)
    code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                   depth_minus_w=DEPTH_MINUS_W, **answers)
    if stage == 'cs':
        request['local'] = list(struct.unpack_from('<III', control, 0x748))
    return request


class TranslationCache:
    """Each job's result by variant name in <output>.translations: <variant>_b.metal / _b.json (hard links to the seed's
    files), or <variant>.failed with the error. cache.json names what a result depends on besides the job; a different
    identity empties the folder. A list that grew then translates only its new rows."""

    def __init__(self, folder, identity):
        self.folder = folder
        try:
            same = json.loads((folder / 'cache.json').read_text()) == identity
        except (OSError, ValueError):
            same = False
        if not same:
            shutil.rmtree(folder, ignore_errors=True)
            folder.mkdir(parents=True)
            (folder / 'cache.json').write_text(json.dumps(identity))

    def take(self, variant, staging):
        failed = self.folder / (variant + '.failed')
        if failed.is_file():
            return dict(ok=False, error=json.loads(failed.read_text()).get('error'))
        names = [variant + '_b.metal', variant + '_b.json']
        if not all((self.folder / n).is_file() and (self.folder / n).stat().st_size for n in names):
            return None
        for name in names:
            (staging / name).unlink(missing_ok=True)
            os.link(self.folder / name, staging / name)
        return dict(ok=True)

    def put(self, variant, reply, staging):
        if not reply.get('ok'):
            (self.folder / (variant + '.failed')).write_text(json.dumps(dict(error=reply.get('error'))))
            return
        for name in (variant + '_b.metal', variant + '_b.json'):
            (self.folder / name).unlink(missing_ok=True)
            os.link(staging / name, self.folder / name)

    def prune(self, variants):
        for p in self.folder.iterdir():
            for end in ('_b.metal', '_b.json', '.failed'):
                if p.name.endswith(end) and p.name[:-len(end)] not in variants:
                    p.unlink()


def translate(shaders, jobs, staging, cache, translator, spirv_cross, workers):
    """Run the jobs on up to `workers` translator services, one shader's jobs in order on one service (whether a listed
    job is needed depends on the variants made before it). -> {key: [(variant, deps)]}, [failure records], reused."""
    by_key = {}
    for job in jobs:
        by_key.setdefault(job[0], []).append(job)
    variants, failures, lock, stop = {}, [], threading.Lock(), threading.Event()
    pending, counts, started = iter(sorted(by_key.items())), [0, 0], time.monotonic()

    def worker():
        process = None
        try:
            while not stop.is_set():
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, mine_jobs = item
                stage, control, code, source = shaders[key]
                mine = {}   # canonical deps -> variant
                for _, answers, variant, why in mine_jobs:
                    if why != 'default' and any(deps_match(json.loads(d), answers) for d in mine):
                        continue
                    reply = cache.take(variant, staging)
                    reused = reply is not None
                    if not reused:
                        if process is None:
                            process = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross),
                                                        '--out', str(staging), '--chain', CHAIN],
                                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                        code_path = staging / (variant + '.code')
                        request = translation_request(variant, stage, control, code, answers, code_path)
                        process.stdin.write(canonical(request) + '\n')
                        process.stdin.flush()
                        line = process.stdout.readline()
                        reply = json.loads(line) if line else dict(ok=False, error='the translator service exited')
                        code_path.unlink(missing_ok=True)
                        for extra in ('.spv', '.res.json'):   # the translator's intermediates: not runtime inputs
                            (staging / (variant + extra)).unlink(missing_ok=True)
                        if not line:   # a new service for the next job; this one is recorded as failed
                            process.wait()
                            process = None
                        else:
                            cache.put(variant, reply, staging)
                    if not reply.get('ok'):
                        with lock:
                            failures.append(dict(key=key, variant=variant, why=why, stage=stage, source=source,
                                                 error=str(reply.get('error'))[:400]))
                        continue
                    refl = json.loads((staging / (variant + '_b.json')).read_text())
                    if refl.get('stage') != stage:
                        raise SeedError(f'{variant}: the translated stage is {refl.get("stage")}, not {stage}')
                    deps = canonical(refl.get('deps', {}))
                    if deps not in mine:
                        mine[deps] = variant
                    with lock:
                        counts[0] += 1
                        counts[1] += reused
                        if counts[0] % 1000 == 0:
                            print(f'  {counts[0]} translated ({counts[1]} from the translation cache), '
                                  f'{time.monotonic() - started:.0f} s', flush=True)
                if mine:
                    with lock:
                        variants[key] = [(v, json.loads(d)) for d, v in mine.items()]
        except BaseException:
            stop.set()
            raise
        finally:
            if process is not None:
                process.stdin.close()
                process.wait()

    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for future in [pool.submit(worker) for _ in range(workers)]:
            future.result()
    return variants, sorted(failures, key=lambda f: f['variant']), counts[1]


def judge(failures):
    """Failures -> the excluded records (key, reason, result). Any failure of a key not in EXCLUDED stops the run."""
    unexpected = [f for f in failures if f['key'] not in EXCLUDED]
    if unexpected:
        for f in unexpected[:10]:
            print(f'  translation failed: {f["variant"]} ({f["source"]}): {f["error"][:200]}', flush=True)
        raise SeedError(f'{len(unexpected)} translations failed; the published seed is unchanged')
    def result(key):
        return next((f['error'] for f in failures if f['key'] == key), 'translated: now included')
    return [dict(key=k, reason=r, result=result(k)) for k, r in sorted(EXCLUDED.items())]


def finish(staging, files_wanted, manifest):
    """Make staging a complete seed: only the wanted files, and seed-manifest.json naming each with its SHA-256."""
    for p in staging.iterdir():   # leftovers of failed or duplicate translations: never part of the seed
        if p.name not in files_wanted:
            p.unlink()
    manifest['files'] = {name: file_digest(staging / name) for name in sorted(files_wanted)}
    (staging / 'seed-manifest.json').write_text(json.dumps(manifest, indent=1, sort_keys=True) + '\n')


def publish(staging, output, libs, workers, metal=None):
    """Compile the complete seed's libraries beside the published ones, then publish both: the libraries, then the
    seed, the old generation kept in the .partial folders until both exchanges are done. If either fails or the run is
    interrupted (also just after an exchange, before the call returned: the folder at each path is told by its inode),
    every path that holds a new folder gets its old one back. Not covered: a second interrupt during that rollback, or
    a kill; `build_shader_libs.py --check` names the mismatch and `make shader-seed` repairs it. libs=None: the seed
    alone (development runs)."""
    built = libraries_module()
    if libs is None:
        built.swap_in(staging, output)
        return 0
    try:
        libs_staging, count = built.stage_seed_libs(libs, staging, workers, metal)
    except built.LibsError as error:
        raise SeedError(f'shader libraries: {error}; the published seed and libraries are unchanged') from None
    new_libs, new_seed = libs_staging.stat().st_ino, staging.stat().st_ino
    had_libs, had_seed = libs.exists(), output.exists()

    def put_back(path, staged, new, had):
        """If path holds the new folder (its exchange happened, even when an interrupt came before the call
        returned), the old one goes back: exchanged with staged, or moved away when there was none."""
        if path.exists() and path.stat().st_ino == new:
            if had:
                built.exchange(staged, path)
            else:
                path.rename(staged)
    try:
        built.exchange(libs_staging, libs)   # the old libraries (if any) are now at libs_staging
        built.exchange(staging, output)      # the old seed (if any) is now at staging
    except BaseException as error:
        put_back(output, staging, new_seed, had_seed)
        put_back(libs, libs_staging, new_libs, had_libs)
        shutil.rmtree(libs_staging, ignore_errors=True)
        if isinstance(error, OSError):
            raise SeedError(f'publishing the seed failed ({error}); the published seed and libraries are unchanged') \
                from None
        raise
    shutil.rmtree(libs_staging, ignore_errors=True)
    shutil.rmtree(staging, ignore_errors=True)
    return count


def prepare(work, output, workers, list_path=LIST, libs=True, metal=None):
    started = time.monotonic()
    translator, spirv_cross = work / 'build/shader-service/totk_xlate', work / 'build/spirv-cross/spirv-cross'
    for tool in (translator, spirv_cross):
        if not os.access(tool, os.X_OK):
            raise SeedError(f'{tool} is missing (make shader-tools)')
    romfs = work / 'extracted' / VERSION / 'romfs'
    shaders, archives = inventory(romfs, work / 'rewritten' / VERSION)
    found = census(shaders)
    print(f'{found["shaders"]} shaders ({found["vs"]} vertex, {found["ps"]} pixel, {found["cs"]} compute) in '
          f'{len(archives) + 1} files, {time.monotonic() - started:.0f} s', flush=True)
    listed = read_list(list_path)
    jobs = plan(shaders, listed)
    staging = output.with_name(output.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        cache = TranslationCache(output.with_name(output.name + '.translations'), dict(
            schema=1, chain=CHAIN, translator_sha256=file_digest(translator),
            spirv_cross_sha256=file_digest(spirv_cross), depth_minus_w=DEPTH_MINUS_W,
            request_sha256=digest(inspect.getsource(translation_request).encode())))
        variants, failures, reused = translate(shaders, jobs, staging, cache, translator, spirv_cross, workers)
        excluded = judge(failures)
        wanted = set()
        for key, rows in variants.items():
            (staging / f'{key}.variants').write_text(canonical([dict(variant=v, deps=d) for v, d in rows]))
            wanted.add(f'{key}.variants')
            wanted.update(f'{v}{end}' for v, _ in rows for end in ('_b.metal', '_b.json'))
        count = sum(map(len, variants.values()))
        manifest = dict(schema=1, chain=CHAIN, shaders=len(variants), variants=count, census=found, version=VERSION,
                        romfs_sha256=json.loads((romfs / 'index.json').read_text()).get('romfs_sha256'),
                        listed_rows=len(listed), listed_jobs=sum(1 for j in jobs if j[3] == 'listed'),
                        excluded=excluded, depth_minus_w=DEPTH_MINUS_W,
                        script_sha256=file_digest(Path(__file__)))
        finish(staging, wanted, manifest)
        libraries = publish(staging, output, work / 'build/shader-libs' if libs else None, os.cpu_count() or 4, metal)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    cache.prune({j[2] for j in jobs})
    print(f'shader seed: {len(variants)} of {found["shaders"]} shaders, {count} variants ({manifest["listed_jobs"]} '
          f'listed bind states), {len(excluded)} excluded; {reused} from the translation cache; '
          + (f'{libraries} libraries; ' if libs else 'libraries not built (--no-libs); ')
          + f'{time.monotonic() - started:.0f} s -> {output}', flush=True)
    return manifest


def add_listed(seed, played, list_path=LIST):
    """Add to the list each bind state of a played cache that no seed variant serves (shader_list.py add)."""
    try:
        added = shader_list.add(list_path, seed, played)
    except shader_list.ListError as error:
        raise SeedError(str(error)) from None
    print(f'{added} bind states added; {len(read_list(list_path))} in {list_path}')
    return added


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, required=True, help='the work folder (extracted/, rewritten/, build/)')
    parser.add_argument('--output', type=Path, help='the seed folder (default: <work>/build/shader-seed)')
    parser.add_argument('--jobs', type=int, default=min(8, os.cpu_count() or 4), help='translator services')
    parser.add_argument('--shader-list', type=Path, default=LIST)
    parser.add_argument('--list-from', type=Path, nargs='+',
                        help='played shader caches whose new bind states join the list')
    parser.add_argument('--no-libs', action='store_true',
                        help='publish the seed without compiling its libraries (build_shader_libs.py --seed later)')
    parser.add_argument('--metal', help=argparse.SUPPRESS)   # tests: a stand-in compiler
    args = parser.parse_args(argv)
    work = args.work.resolve()
    output = (args.output or work / 'build/shader-seed').resolve()
    try:
        if args.list_from:
            add_listed(output, [p.resolve() for p in args.list_from], args.shader_list.resolve())
        else:
            prepare(work, output, max(1, args.jobs), args.shader_list.resolve(), not args.no_libs, args.metal)
    except SeedError as error:
        sys.exit(f'shader seed: {error}')


if __name__ == '__main__':
    main()
