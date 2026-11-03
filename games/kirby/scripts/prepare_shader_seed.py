#!/usr/bin/env python3
"""Translate Kirby's shaders from your own game files when the app is built, so play does not stop to translate them.

    prepare_shader_seed.py --work Kirby_build [--jobs N]
    prepare_shader_seed.py --work Kirby_build --list-from "Kirby Data/shader-cache"

Every shader the game carries is found: the BNSH archives in the RomFS (most inside HAL's .cmp containers, a 4-byte
uncompressed size and then a zstd frame) and in the main module. Its key is the runtime's (nvn_mtl.m prog_get: SHA-256
of the control section up to ctl_key_len, then the code). Each is translated with build/shader-service/totk_xlate for
the bind state the game draws it with:
 - vertex shaders: every attribute a float (the game binds its integer inputs, such as bone indices, as *_UI2F formats
   that Metal converts), clip depth -w..w (the game selects NVN_DEPTH_MODE_NEAR_IS_MINUS_W before its first draw);
 - pixel and compute shaders: plain 2D textures, float colour targets; a pixel shader that writes colour target 5 also
   gets the variant with target 5 an unsigned integer (the six-target G-buffer pass);
 - a shader that compare-samples textures (shadow maps) also gets the variant with those slots bound to depth textures;
 - and every bind state the V rows of assets/shader_list.tsv (the shared list, docs/shader-list.md) name for it: shader
   key and deps met in play, needed for what the code alone does not show (a plain sample of a depth texture, an
   integer vertex input).
The runtime picks a variant by its deps (totk_shader_rt.m deps_match), not by its name, so a variant made here serves
every draw whose bind state matches its deps. Measured on a played cache of 1,449 variants: every variant made here
with the same deps as the played one had byte-identical MSL, so the libraries compiled from it are the ones the
runtime looks up (scripts/precompile_cached_shaders.py, by SHA-256 of the MSL).

The output, <work>/build/shader-seed, is in the runtime's seed format (seed-manifest.json, <key>.variants and
<variant>_b.metal / _b.json); the app imports it into "Kirby Data/shader-cache" at its first start (totk_shader_seed.h).
It holds game-derived data: it stays in the work folder and the app, never in the repository.

--list-from adds to assets/shader_list.tsv the bind states a played shader cache met that the seed does not serve.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time

from compression import zstd
import lz4.block

GAME_DIR = Path(__file__).resolve().parents[1]
LIST = GAME_DIR / 'assets/shader_list.tsv'
CHAIN = re.search(r'#define DMSH_CHAIN "([^"]+)"', (GAME_DIR / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h').read_text()).group(1)
SCHEMA = 1
ZSTD = b'\x28\xb5\x2f\xfd'
STAGES = (('vs', 8, 0), ('ps', 40, 1), ('cs', 48, 5))   # stage, offset in a BNSH program, NVN stage at control+0x714
EMPTY = dict(tex={}, img={}, attr={}, fout={})
VERTEX = dict(EMPTY, attr={str(i): 'float' for i in range(16)})
DEFAULTS = dict(tex='2d', img={}, attr='float', fout='float')
BROKEN = (b'double ', b'unsupported_GLSLstd450')   # MSL the runtime translates again (totk_shader_rt.m)
MIN_SHADERS = 20000   # the base game has 27,293 (27,239 in the RomFS, 54 in main)
HEADER = '# Kirby shader list: shader keys and bind states only, no game data (V <shader key> <deps>; docs/shader-list.md).\n'


class SeedError(Exception):
    pass


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def compact(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def variant_name(key, stage, answers):
    """The runtime's name for a translation (totk_rt_variant): the key and the SHA-256 of the answers it was asked with."""
    asked = dict(answers, depth_minus_w=True) if stage == 'vs' else answers
    return f'{key}-{digest(compact(asked).encode())[:12]}'


def deps_match(deps, answers):
    """totk_shader_rt.m deps_match: whether a variant translated with deps serves a draw with these answers."""
    for kind, default in DEFAULTS.items():
        have = answers.get(kind, {})
        for slot, want in deps.get(kind, {}).items():
            if slot not in have and kind == 'attr':
                continue
            got = have.get(slot, default)
            if want in ('*', '*_depth'):
                if not isinstance(got, str) or got.endswith('_depth') != (want == '*_depth'):
                    return False
            elif got != want:
                return False
    return True


def answers_for(stage, deps):
    """The bind state to translate a listed row with: its deps over the stage's defaults, "*" as the 2D texture it stands for."""
    answers = {**(VERTEX if stage == 'vs' else EMPTY), **deps}
    answers['tex'] = {slot: {'*': '2d', '*_depth': '2d_depth'}.get(kind, kind) for slot, kind in answers['tex'].items()}
    return answers


# ---- finding the shaders ----
def control_key_len(control):
    """nvn_mtl.m ctl_key_len: the control section's own size (the word at +0x20) when plausible, else 0x880."""
    size = struct.unpack_from('<I', control, 0x20)[0]
    return size if 0x7cc <= size < 0x880 else 0x880


def bnsh_shaders(data, base):
    """(key, stage, control, code) of the vertex, pixel and compute shaders of the BNSH at data[base:]. Geometry shaders
    are left out: the runtime has no translation chain for them."""
    if data[base + 12:base + 14] != b'\xff\xfe' or data[base + 96:base + 100] != b'grsc':
        return
    size = struct.unpack_from('<I', data, base + 28)[0]
    blob = data[base:base + size]
    if len(blob) != size:
        raise ValueError('BNSH runs past its file')
    count, variations = struct.unpack_from('<IQ', blob, 124)
    for i in range(count):
        program = struct.unpack_from('<Q', blob, variations + i * 64 + 16)[0]
        for stage, relative, nvn_stage in STAGES:
            offset = struct.unpack_from('<Q', blob, program + relative)[0]
            if not offset:
                continue
            control_off, code_off, _, control_size = struct.unpack_from('<QQII', blob, offset + 8)
            control = blob[control_off:control_off + control_size]
            if len(control) < 0x7cc or len(control) < control_key_len(control):
                continue   # the SDK's own utility shaders use an older, shorter control layout
            control = control[:control_key_len(control)]
            c1, length, actual = struct.unpack_from('<II', control, 0x700) + struct.unpack_from('<I', control, 0x714)
            code = blob[code_off:code_off + length]
            if actual != nvn_stage or not 0 < length == len(code) or c1 > length:
                raise ValueError('invalid NVN shader bounds or stage')
            yield digest(control + code), stage, control, code


def unpacked(raw):
    """A file's bytes with HAL's containers opened: a zstd frame, at 0 or after a 4-byte size. LZ4 containers (map data
    and sound, ubin/ and snd/) hold no shaders and are read as they are."""
    if raw[:4] == ZSTD:
        return zstd.decompress(raw)
    if raw[4:8] == ZSTD:
        data = zstd.decompress(raw[4:])
        if len(data) != struct.unpack_from('<I', raw)[0]:
            raise ValueError('zstd container size differs from its prefix')
        return data
    return raw


def nso_image(path):
    """The main module's memory image (third_party/d2r/nsoimg.py load)."""
    d = path.read_bytes()
    if d[:4] != b'NSO0':
        raise SeedError(f'{path}: not an NSO')
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segments = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    sizes = struct.unpack_from('<III', d, 0x60)
    image = bytearray(segments[2][1] + segments[2][2])
    for i, (file_off, mem_off, size) in enumerate(segments):
        raw = d[file_off:file_off + sizes[i]]
        image[mem_off:mem_off + size] = lz4.block.decompress(raw, uncompressed_size=size) if flags >> i & 1 else raw
    return bytes(image)


def scan(job):
    """Write the shaders of one source to <sources>/<key>.<stage>; returns [(key, stage)], or an error."""
    sources, label, romfs, offset, size, module = job
    try:
        if module:
            data = nso_image(Path(module))
        else:
            with open(romfs, 'rb') as f:
                f.seek(offset)
                data = unpacked(f.read(size))
        found = []
        at = data.find(b'BNSH')
        while at >= 0:
            for key, stage, control, code in bnsh_shaders(data, at):
                out = Path(sources) / f'{key}.{stage}'
                if not out.exists():
                    temp = out.with_name(f'{out.name}.{os.getpid()}')
                    temp.write_bytes(struct.pack('<I', len(control)) + control + code)
                    temp.replace(out)
                found.append((key, stage))
            at = data.find(b'BNSH', at + 4)
        return label, found, None
    except (ValueError, struct.error, zstd.ZstdError, OSError) as error:
        return label, [], f'{type(error).__name__}: {error}'


def find_shaders(work, sources, jobs):
    index = json.loads((work / 'romfs/index.json').read_text())
    romfs = str(work / 'romfs/romfs.bin')
    work_list = [(str(sources), f['path'], romfs, f['offset'], f['size'], None) for f in index['files']]
    work_list.append((str(sources), 'module:main', None, 0, 0, str(work / 'extracted/exefs/main')))
    shaders, problems = {}, []
    with ProcessPoolExecutor(jobs) as pool:
        for label, found, error in pool.map(scan, work_list, chunksize=8):
            if error:
                problems.append(f'{label}: {error}')
            for key, stage in found:
                shaders.setdefault(key, stage)
    return shaders, problems


# ---- translating ----
class Translator:
    """Services of build/shader-service/totk_xlate (the runtime's own translator), one per worker thread."""
    def __init__(self, work, folder, jobs):
        self.command = [str(work / 'build/shader-service/totk_xlate'), '--service', '--spirv-cross',
                        str(work / 'build/spirv-cross/spirv-cross'), '--out', str(folder), '--chain', CHAIN]
        self.folder, self.jobs, self.sources = folder, jobs, None

    def request(self, key, stage, answers, variant):
        """The request the runtime sends (totk_rt_variant), with the guest code written beside the translation."""
        raw = (self.sources / f'{key}.{stage}').read_bytes()
        n = struct.unpack_from('<I', raw)[0]
        control, code = raw[4:4 + n], raw[4 + n:]
        c1, length = struct.unpack_from('<II', control, 0x700)
        code_path = self.folder / f'{variant}.code'
        code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
        request = dict(variant=variant, stage=stage, code=str(code_path), c1=code[c1:length].hex(),
                       depth_minus_w=stage == 'vs', **answers)
        if stage == 'cs':
            request['local'] = list(struct.unpack_from('<III', control, 0x748))
        return request

    def run(self, jobs, progress):
        """Translate (key, stage, answers, variant) jobs not translated before; returns {variant: error} of failures."""
        todo = queue.Queue()
        for job in jobs:
            if not ((self.folder / f'{job[3]}_b.metal').is_file() and (self.folder / f'{job[3]}_b.json').is_file()):
                todo.put(job)
        total, failed, lock, done = todo.qsize(), {}, threading.Lock(), [0]

        def worker():
            service = subprocess.Popen(self.command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
            try:
                while True:
                    try:
                        key, stage, answers, variant = todo.get_nowait()
                    except queue.Empty:
                        return
                    service.stdin.write(compact(self.request(key, stage, answers, variant)) + '\n')
                    service.stdin.flush()
                    line = service.stdout.readline()
                    reply = json.loads(line) if line else {'error': 'translator exited'}
                    with lock:
                        if not reply.get('ok'):
                            failed[variant] = str(reply.get('error'))
                        done[0] += 1
                        if done[0] % 2000 == 0 or done[0] == total:
                            progress(f'   translated {done[0]:,} of {total:,}')
                    if not line:
                        return
            finally:
                service.stdin.close()
                service.wait()
        threads = [threading.Thread(target=worker) for _ in range(min(self.jobs, max(total, 1)))]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        if todo.qsize():
            raise SeedError(f'the translator stopped with {todo.qsize()} shaders left (first failure: {next(iter(failed.items()), None)})')
        return failed


def reflection(folder, variant):
    return json.loads((folder / f'{variant}_b.json').read_text())


def follow_ups(key, stage, refl):
    """The variants a translation shows the game also needs: colour target 5 as an unsigned integer (G-buffer), and the
    compare-sampled slots bound to depth textures (shadow maps), each alone and together."""
    options = [[{}]]
    if stage == 'ps' and '5' in refl.get('deps', {}).get('fout', {}):
        options.append([{}, {'fout': {'5': 'uint'}}])
    if refl.get('compare_slots'):
        options.append([{}, {'tex': {str(s): '2d_depth' for s in refl['compare_slots']}}])
    out = [{}]
    for choices in options:
        out = [{**a, **b} for a in out for b in choices]
    base = VERTEX if stage == 'vs' else EMPTY
    return [{**base, **extra} for extra in out if extra]


def read_list(path):
    """The V rows (shader key, deps) of the shared list; its warm-up rows (S, C, R, ...) are not shaders to translate."""
    rows = []
    if path.is_file():
        for n, line in enumerate(path.read_text().splitlines(), 1):
            if not line.startswith('V\t'):
                continue
            _, key, deps = line.split('\t')
            if not re.fullmatch(r'[0-9a-f]{64}', key):
                raise SeedError(f'{path}:{n}: expected V <shader key> <deps>')
            rows.append((key, json.loads(deps)))
    return rows


def identity_of(work, shader_list):
    index = json.loads((work / 'romfs/index.json').read_text())
    return dict(schema=SCHEMA, chain=CHAIN, romfs_sha256=index['romfs_sha256'],
                main_sha256=file_digest(work / 'extracted/exefs/main'),
                translator_sha256=file_digest(work / 'build/shader-service/totk_xlate'),
                spirv_cross_sha256=file_digest(work / 'build/spirv-cross/spirv-cross'),
                script_sha256=file_digest(Path(__file__)),
                list_sha256=file_digest(shader_list) if shader_list.is_file() else None)


def translation_identity(identity):
    """What a translation depends on; a change of this script alone keeps the translations folder."""
    return {k: identity[k] for k in ('chain', 'translator_sha256', 'spirv_cross_sha256')}


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def prepare(work, output, jobs, shader_list=LIST):
    identity = identity_of(work, shader_list)
    stamp = output.with_name(output.name + '.identity.json')
    if stamp.is_file() and json.loads(stamp.read_text()) == identity and (output / 'seed-manifest.json').is_file():
        manifest = json.loads((output / 'seed-manifest.json').read_text())
        say(f'shader seed up to date: {manifest["shaders"]:,} shaders, {len(manifest["files"]) // 2:,} variants')
        return manifest
    translations = work / 'build/shader-translations'
    want = translation_identity(identity)
    marker = translations / 'identity.json'
    if translations.exists() and (not marker.is_file() or json.loads(marker.read_text()) != want):
        shutil.rmtree(translations)
    translations.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps(want, sort_keys=True) + '\n')
    sources = work / 'build/shader-sources'
    shutil.rmtree(sources, ignore_errors=True)
    sources.mkdir(parents=True)
    try:
        t0 = time.time()
        shaders, problems = find_shaders(work, sources, jobs)
        say(f'found {len(shaders):,} shaders in {time.time() - t0:.0f} s')
        for problem in problems[:10]:
            say(f'   unreadable: {problem}')
        if problems:
            raise SeedError(f'{len(problems)} game files could not be read')
        if len(shaders) < MIN_SHADERS:
            raise SeedError(f'only {len(shaders)} shaders found, expected about 27,000')
        translator = Translator(work, translations, jobs)
        translator.sources = sources
        variants = {key: [] for key in shaders}   # key -> [(variant, deps)]
        failed = {}

        def translate(planned, what):
            planned = [job for job in planned if not any(job[3] == v for v, _ in variants[job[0]])]
            t = time.time()
            failed.update(translator.run(planned, say))
            kept = 0
            for key, stage, answers, variant in planned:
                if variant in failed:
                    continue
                msl = (translations / f'{variant}_b.metal').read_bytes()
                deps = reflection(translations, variant).get('deps', {})
                if any(marker in msl for marker in BROKEN) or any(d == deps for _, d in variants[key]):
                    continue
                variants[key].append((variant, deps))
                kept += 1
            say(f'{what}: {kept:,} variants ({time.time() - t:.0f} s)')

        default = [(key, stage, VERTEX if stage == 'vs' else EMPTY) for key, stage in sorted(shaders.items())]
        translate([(k, s, a, variant_name(k, s, a)) for k, s, a in default], 'default bind state')
        extra = []
        for key, stage in sorted(shaders.items()):
            if variants[key]:
                refl = reflection(translations, variants[key][0][0])
                extra += [(key, stage, a, variant_name(key, stage, a)) for a in follow_ups(key, stage, refl)]
        translate(extra, 'G-buffer and shadow-map bind states')
        listed = []
        for key, deps in read_list(shader_list):
            if key not in shaders:
                continue
            answers = answers_for(shaders[key], deps)
            if not any(deps_match(d, answers) for _, d in variants[key]):
                listed.append((key, shaders[key], answers, variant_name(key, shaders[key], answers)))
        translate(listed, f'listed bind states ({shader_list.name})')
        for variant, error in sorted(failed.items())[:10]:
            say(f'   translation failed: {variant}: {error[:200]}')
        if len(failed) > len(shaders) // 100:
            raise SeedError(f'{len(failed)} translations failed')
        return publish(output, translations, shaders, variants, identity, stamp)
    finally:
        shutil.rmtree(sources, ignore_errors=True)


def publish(output, translations, shaders, variants, identity, stamp):
    """The seed folder in the runtime's format, written beside the old one and swapped in."""
    staging = output.with_name(output.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    files = {}
    for key in sorted(variants):
        if not variants[key]:
            continue
        rows = [dict(variant=v, deps=d, depth_minus_w=shaders[key] == 'vs') for v, d in variants[key]]
        (staging / f'{key}.variants').write_text(compact(rows))
        files[f'{key}.variants'] = None
        for variant, _ in variants[key]:
            for suffix in ('_b.metal', '_b.json'):
                os.link(translations / f'{variant}{suffix}', staging / f'{variant}{suffix}')
                files[f'{variant}{suffix}'] = None
    for name in files:
        files[name] = file_digest(staging / name)
    count = sum(1 for v in variants.values() if v)
    manifest = dict(schema=1, chain=CHAIN, shaders=count, files=files)
    (staging / 'seed-manifest.json').write_text(json.dumps(manifest, sort_keys=True) + '\n')
    stamp.unlink(missing_ok=True)
    if output.exists():
        shutil.rmtree(output)
    staging.replace(output)
    stamp.write_text(json.dumps(identity, sort_keys=True) + '\n')
    say(f'shader seed: {count:,} shaders, {sum(map(len, variants.values())):,} variants in {output}')
    return manifest


def add_listed(seed, played, shader_list=LIST):
    """Add to the list each bind state of a played cache that no seed variant of its shader serves."""
    rows = {(key, compact(deps)) for key, deps in read_list(shader_list)}
    lines = shader_list.read_text().splitlines(keepends=True) if shader_list.is_file() else [HEADER]
    added = 0
    for index in sorted(played.glob('*.variants')):
        key = index.name[:64]
        mine = seed / index.name
        have = [r['deps'] for r in json.loads(mine.read_text())] if mine.is_file() else None
        if have is None:
            continue   # not a shader the seed knows: nothing to translate it from
        for row in json.loads(index.read_text()):
            deps = row.get('deps', {})
            if not any(deps_match(d, answers_for('', deps)) for d in have) and (key, compact(deps)) not in rows:
                rows.add((key, compact(deps)))
                lines.append(f'V\t{key}\t{compact(deps)}\n')
                added += 1
    shader_list.write_text(''.join(lines))
    print(f'{added} bind states added; {len(rows)} in {shader_list}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--work', type=Path, required=True, help='the build work folder (scripts/build_kirby.py --work)')
    parser.add_argument('--output', type=Path, help='the seed folder (default: <work>/build/shader-seed)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4, help='translator services and scan processes')
    parser.add_argument('--list-from', type=Path, help='a played shader cache whose bind states the seed misses go into the list')
    args = parser.parse_args(argv)
    work = args.work.resolve()
    output = (args.output or work / 'build/shader-seed').resolve()
    try:
        if args.list_from:
            add_listed(output, args.list_from.resolve())
        else:
            prepare(work, output, max(1, args.jobs))
    except SeedError as error:
        sys.exit(f'shader seed: {error}')


if __name__ == '__main__':
    main()
