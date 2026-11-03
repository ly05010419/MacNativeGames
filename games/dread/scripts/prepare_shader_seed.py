#!/usr/bin/env python3
"""Prepare Metroid Dread's shaders from the verified local RomFS, for the iPhone / iPad (which cannot translate).

Forked from games/totk/scripts/prepare_shader_seed.py for this folder's older runtime (translation chain
totk-native-v4-unverified, third_party/d2r_gfx/gfx/nvn/dmsh_chain.h) and the game's own shader container:
  - every shader sits in system/shd/*.bshdat (MercurySteam's MSHD files): an NVN control section (magic 0x98761234, its
    0x880 bytes hashed into the key), the shader's assembly text, then its code (magic 0x12345678). A scan of every RomFS
    file and the game modules found NVN control sections nowhere else (2026-11-02: 1,342 sections, 1,010 unique shaders);
  - each shader is translated for the default bind state, then for every state assets/shader_list.tsv names for it, as
    the runtime would answer them (totk_shader_rt.m totk_rt_variant: the variant is the shader key plus the SHA-256 of
    its answers as compact sorted JSON; deps_match picks a variant without wildcards, defaults 2d / float);
  - the seed holds only the files the runtime's import accepts (totk_shader_seed.h): <key>.variants,
    <variant>_b.metal / _b.json and pso_manifest.tsv, named in seed-manifest.json with their SHA-256.
The output contains game-derived data and belongs in the work folder, never in the repo.

  prepare_shader_seed.py --romfs <work>/extracted/v100/romfs --output <work>/build/shader-seed \\
      --translator <work>/build/shader-service/totk_xlate --spirv-cross <work>/build/spirv-cross/spirv-cross --version v100
"""
import argparse
import concurrent.futures
import hashlib
import inspect
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from shader_list import ShaderList   # noqa: E402

CHAIN = 'totk-native-v4-unverified'
MIN_SHADERS = 900   # of the 1,010 unique shaders of 1.0.0: fewer means the scan or the translator went wrong
LIST = Path(__file__).resolve().parents[1]/'assets/shader_list.tsv'
CONTROL_MAGIC = struct.pack('<I', 0x98761234)
CODE_MAGIC = struct.pack('<I', 0x12345678)
CONTROL_SIZE = 0x880
STAGES = {0: 'vs', 1: 'ps', 5: 'cs'}
ANSWERS = dict(tex={}, img={}, attr={}, fout={})
DEFAULTS = dict(tex='2d', img={}, attr='float', fout='float')   # totk_shader_rt.m deps_match
DEPTH_MINUS_W = True   # nvnDeviceSetDepthMode NEAR_IS_MINUS_W: the Mac's own vertex translations have the clip-z conversion


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'))


def variant_name(key, answers):
    """totk_shader_rt.m totk_rt_variant: the key and the first 6 bytes of the SHA-256 of the answers' sorted JSON."""
    return key+'-'+digest(canonical(answers).encode())[:12]


def deps_match(deps, answers):
    """Whether a variant translated with deps serves these answers (totk_shader_rt.m deps_match): every slot the deps
    name must be answered the same, an unanswered slot counting as its kind's default."""
    for kind, default in DEFAULTS.items():
        have = answers.get(kind, {})
        for slot, want in deps.get(kind, {}).items():
            if have.get(slot, default) != want:
                return False
    return True


def answers_for(deps):
    """The bind state to translate a listed row with: its deps, every other kind empty."""
    return {**ANSWERS, **deps}


def mshd_shaders(data):
    """The shaders of one .bshdat file -> (key, stage, control, code). Each control section is followed by the shader's
    assembly text and then its code; the code is the first 4-aligned code header after the section (this pairing gave
    the key of each of the 190 shaders a Mac had translated, 2026-11-02). A wrong pairing cannot be used: its key would
    be no shader the game binds."""
    if data[:4] != b'MSHD':
        raise ValueError('expected an MSHD shader file')
    seen = set()
    at = 0
    while (at := data.find(CONTROL_MAGIC, at)) >= 0:
        control_at, at = at, at + 4
        if control_at % 4 or control_at + CONTROL_SIZE > len(data):
            continue
        control = data[control_at:control_at+CONTROL_SIZE]
        c1, length = struct.unpack_from('<II', control, 0x700)
        stage = STAGES.get(struct.unpack_from('<I', control, 0x714)[0])
        if not stage or not 0 < length <= 16 << 20 or c1 > length or (stage != 'cs' and c1 < 0x30):
            continue
        code_at = control_at + CONTROL_SIZE
        while (code_at := data.find(CODE_MAGIC, code_at)) >= 0 and code_at % 4:
            code_at += 1
        if code_at < 0 or code_at + length > len(data):
            continue
        code = data[code_at:code_at+length]
        key = digest(control+code)
        if key not in seen:
            seen.add(key)
            yield key, stage, control, code


def inventory(romfs):
    """Every shader of the RomFS -> {key: (stage, control, code, source)}, and the .bshdat files read (path, SHA-256)."""
    index = json.loads((romfs/'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    shaders, archives = {}, []
    with (romfs/'romfs.bin').open('rb') as stream:
        for row in sorted(index['files'], key=lambda r: r['path']):
            if not row['path'].endswith('.bshdat'):
                continue
            stream.seek(row['offset'])
            data = stream.read(row['size'])
            if len(data) != row['size']:
                raise ValueError('short RomFS shader read')
            for key, stage, control, code in mshd_shaders(data):
                shaders.setdefault(key, (stage, control, code, row['path']))
            archives.append(dict(path=row['path'], sha256=digest(data)))
    print(f'discovered {len(shaders)} unique shaders in {len(archives)} .bshdat files', flush=True)
    return shaders, archives


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
    files), or <variant>.failed with the translator's error. cache.json names what a result depends on besides the job
    (the chain, the translator, SPIRV-Cross, translation_request); a different identity empties the folder. A list that
    grew then translates only its new rows."""

    def __init__(self, folder, identity):
        self.folder = folder
        try:
            same = json.loads((folder/'cache.json').read_text()) == identity
        except (OSError, ValueError):
            same = False
        if not same:
            shutil.rmtree(folder, ignore_errors=True)
            folder.mkdir(parents=True)
            (folder/'cache.json').write_text(json.dumps(identity))

    def take(self, variant, staging):
        failed = self.folder/(variant+'.failed')
        if failed.is_file():
            return dict(ok=False, error=json.loads(failed.read_text()).get('error'))
        names = [variant+'_b.metal', variant+'_b.json']
        if not all((self.folder/name).is_file() for name in names):
            return None
        for name in names:
            (staging/name).unlink(missing_ok=True)
            os.link(self.folder/name, staging/name)
        return dict(ok=True)

    def put(self, variant, reply, staging):
        if not reply.get('ok'):
            (self.folder/(variant+'.failed')).write_text(json.dumps(dict(error=reply.get('error'))))
            return
        for name in (variant+'_b.metal', variant+'_b.json'):
            (self.folder/name).unlink(missing_ok=True)
            os.link(staging/name, self.folder/name)

    def prune(self, variants):
        for p in self.folder.iterdir():
            for end in ('_b.metal', '_b.json', '.failed'):
                if p.name.endswith(end) and p.name[:-len(end)] not in variants:
                    p.unlink()


def reusable_seed(output, identity):
    """The seed's manifest when it was prepared with this identity and its files are unchanged, else None."""
    try:
        old = json.loads((output/'seed-manifest.json').read_text())
        if any(old.get(k) != v for k, v in identity.items()) or old.get('shaders', 0) < MIN_SHADERS:
            return None
        if all(digest((output/name).read_bytes()) == sha for name, sha in old['files'].items()):
            return old
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        pass
    return None


def builtin_serves(seed, key, deps):
    """Whether the seed's default variant of a shader serves a list row (key, deps), as the runtime picks variants: such
    a row is not needed in the list (shader_list.py prune)."""
    default = Path(seed)/f'{variant_name(key, ANSWERS)}_b.json'
    try:
        made = json.loads(default.read_text()).get('deps', {})
    except (OSError, ValueError):
        return False
    return deps_match(made, answers_for(json.loads(deps) if isinstance(deps, str) else deps))


def plan(shaders, listed):
    """Jobs (key, answers, variant, why): the default bind state of every shader, then each state the list names for a
    shader of this game."""
    jobs = [(key, ANSWERS, variant_name(key, ANSWERS), 'default') for key in sorted(shaders)]
    for key, deps in sorted(listed):
        if key in shaders:
            answers = answers_for(json.loads(deps))
            jobs.append((key, answers, variant_name(key, answers), 'listed'))
    return jobs


def prepare(romfs, output, translator, spirv_cross, version, shader_list=None):
    shader_list = Path(shader_list or LIST)
    listed = ShaderList.load(shader_list).variants
    identity = dict(schema=1, chain=CHAIN, version=version, game='dread',
                    source_romfs_sha256=json.loads((romfs/'index.json').read_text())['romfs_sha256'],
                    translator_sha256=digest(translator.read_bytes()),
                    spirv_cross_sha256=digest(spirv_cross.read_bytes()),
                    preparation_sha256=digest(Path(__file__).read_bytes()),
                    shader_list_sha256=digest(shader_list.read_bytes()) if shader_list.is_file() else None)
    old = reusable_seed(output, identity)
    if old is not None:
        print(f'reusing the prepared seed ({old["shaders"]} shader variants)', flush=True)
        return old
    started = time.monotonic()
    staging = output.with_name(output.name+'.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    shaders, archives = inventory(romfs)
    cache = TranslationCache(output.with_name(output.name+'.translations'), dict(
        schema=1, chain=CHAIN, translator_sha256=identity['translator_sha256'],
        spirv_cross_sha256=identity['spirv_cross_sha256'],
        request_sha256=digest(inspect.getsource(translation_request).encode())))
    jobs = plan(shaders, listed)
    by_key = {}
    for index, job in enumerate(jobs):
        by_key.setdefault(job[0], []).append(index)
    outcome = [None]*len(jobs)   # ('ok', variant, stage) / ('failed', record) / None (served by an earlier variant)
    lock = threading.Lock()
    pending = iter(by_key.items())
    done = [0, 0, 0]   # translated, failed, of them taken from the translation cache
    stop = threading.Event()

    # One translator service per core; a shader's jobs run in order on one service, since whether a listed job is
    # needed depends on the variants made before it. Results are taken in the order of `jobs`.
    def worker():
        process = None
        try:
            while not stop.is_set():
                with lock:
                    item = next(pending, None)
                if item is None:
                    return
                key, indices = item
                stage, control, code, source = shaders[key]
                mine = {}   # canonical deps -> variant
                for index in indices:
                    _, answers, variant, why = jobs[index]
                    if why != 'default' and any(deps_match(json.loads(d), answers) for d in mine):
                        continue
                    reply = cache.take(variant, staging)
                    reused = reply is not None
                    if not reused:
                        if process is None:
                            process = subprocess.Popen([str(translator), '--service', '--spirv-cross', str(spirv_cross),
                                                        '--out', str(staging), '--chain', CHAIN],
                                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
                        code_path = staging/(variant+'.code')
                        request = translation_request(variant, stage, control, code, answers, code_path)
                        process.stdin.write(canonical(request)+'\n'); process.stdin.flush()
                        response = process.stdout.readline()
                        if not response:
                            raise ValueError('shader translation service exited unexpectedly')
                        reply = json.loads(response)
                        code_path.unlink(missing_ok=True)
                        for extra in ('.spv', '.res.json'):   # the translator's intermediates: not runtime inputs
                            (staging/(variant+extra)).unlink(missing_ok=True)
                        cache.put(variant, reply, staging)
                    if reply.get('ok'):
                        refl = json.loads((staging/(variant+'_b.json')).read_text())
                        if refl.get('stage') != stage:
                            raise ValueError(f'{variant}: prepared shader stage mismatch')
                        deps = canonical(refl.get('deps', {}))
                        if deps in mine:   # the same deps as a variant made already: keep one
                            for name in (variant+'_b.metal', variant+'_b.json'):
                                (staging/name).unlink(missing_ok=True)
                            continue
                        mine[deps] = variant
                        outcome[index] = ('ok', variant, stage)
                    else:
                        outcome[index] = ('failed', dict(key=key, variant=variant, why=why, stage=stage, source=source,
                                                         error=reply.get('error')))
                    with lock:
                        done[0 if reply.get('ok') else 1] += 1
                        done[2] += reused
                        if (done[0]+done[1]) % 100 == 0:
                            print(f'prepared {done[0]}/{len(jobs)}, skipped {done[1]}, {done[2]} from the translation '
                                  f'cache; {time.monotonic()-started:.1f}s', flush=True)
                if mine:
                    (staging/(key+'.variants')).write_text(canonical(
                        [dict(variant=v, deps=json.loads(d)) for d, v in mine.items()])+'\n')
        except BaseException:
            stop.set()
            raise
        finally:
            if process is not None:
                process.stdin.close(); process.wait()

    workers = max(1, min(8, os.cpu_count() or 4, len(by_key)))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        for future in [pool.submit(worker) for _ in range(workers)]:
            future.result()
    completed = [r for r in outcome if r and r[0] == 'ok']
    failed = [r[1] for r in outcome if r and r[0] == 'failed']
    ready = len({variant.split('-')[0] for _, variant, _ in completed})
    if ready < MIN_SHADERS:
        raise ValueError(f'only {ready} shaders prepared; at least {MIN_SHADERS} were expected')
    rows = [f'S\t{variant}_b\t'+str({'vs': 0, 'ps': 1, 'cs': 5}[stage]) for _, variant, stage in completed]
    rows += [f'C\t{variant}_b' for _, variant, stage in completed if stage == 'cs']
    (staging/'pso_manifest.tsv').write_text('# Prepared from this app\'s local game data; '+CHAIN+'\n'+'\n'.join(rows)+'\n')
    wanted = {'pso_manifest.tsv'}
    for _, variant, _ in completed:
        wanted.update([variant+'_b.metal', variant+'_b.json', variant.split('-')[0]+'.variants'])
    for p in staging.iterdir():   # failed translations can leave MSL behind: never a cached shader
        if p.name not in wanted:
            p.unlink()
    report = dict(**identity, shaders=len(completed), discovered=len(shaders), startup_shaders=ready, all_default=True,
                  listed_variants=sum(1 for job in jobs if job[3] == 'listed'), skipped=failed, archives=archives,
                  seconds=time.monotonic()-started,
                  files={p.name: digest(p.read_bytes()) for p in sorted(staging.iterdir())})
    (staging/'seed-manifest.json').write_text(json.dumps(report, indent=2)+'\n')
    shutil.rmtree(output, ignore_errors=True)
    staging.rename(output)
    cache.prune({job[2] for job in jobs})
    print(f'shader seed ready: {len(completed)} variants of {ready} shaders ({report["listed_variants"]} listed bind '
          f'states), {len(failed)} skipped; {done[2]} from the translation cache, {done[0]+done[1]-done[2]} translated '
          f'in {report["seconds"]:.0f}s', flush=True)
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ['romfs', 'output', 'translator', 'spirv-cross']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--version', required=True)
    p.add_argument('--shader-list', type=Path, help='default: assets/shader_list.tsv')
    a = p.parse_args()
    prepare(a.romfs.resolve(), a.output.resolve(), a.translator.resolve(), a.spirv_cross.resolve(), a.version,
            a.shader_list.resolve() if a.shader_list else None)


if __name__ == '__main__':
    main()
