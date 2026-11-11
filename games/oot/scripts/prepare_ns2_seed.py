#!/usr/bin/env python3
"""Prepare a Switch 2 game's shader seed for its app (build_app.py; docs/ns2-port.md): the same folder Tears of the
Kingdom's prepare_shader_seed.py writes (build/shader-seed/<version>: *.variants, *_b.metal, *_b.json, shader-keys.json,
seed-manifest.json), so the app's shader steps take it as they are (step_shader_libs, shader_bundle, resolve_lists,
mac_pipelines).

scripts/ns2_shader_corpus.py reads Nintendo's BNSH/BAHS/NDVE resources (OOT: Shader, Lib, Effect, Layout2Shader and Movie),
its modules and, for Koei Tecmo games, LINKDATA and the listed RDB shaders. The game's seed-shaders mode translates each through the
runtime itself (totk_rt_variant), so keys and variant names are the runtime's own: the default bind state for every
shader, and every bind state the game's shared shader list (games/<game>/assets/shader_list.tsv) names, with the answers
a build gives that row. The translations go into a cache kept in the work folder (build/ns2-seed-cache/<version>), so
a rebuild translates only what changed; the seed is that cache's translations, checked and listed in its manifest.

The output is game-derived data: it belongs in the work folder, never in the repository.

  prepare_ns2_seed.py --version oot --depth-mode minus-w --binary ../build/oot-native
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_shader_seed import CHAIN, shader_key_scope, record_verified   # noqa: E402
from ns2_shader_corpus import CORPUS_MAGIC, shader_key

VARIANTS = re.compile(r'^[0-9a-f]{64}\.variants$')
VARIANT = re.compile(r'^[0-9a-f]{64}-[0-9a-f]{12}$')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def translation_context(translator, depth_mode):
    """The cache folder: what decides every translation at once, the chain, the device depth mode and the translator (its
    spirv-cross runs inside it, TOTK_XLATE_SPIRV_CROSS=inproc). Not the runtime code that makes the requests: each
    translation keeps the request it was made from (totk_shader_rt.m variant_request, its index entry's "request"), and a
    seed run of another runtime keeps those it would ask for the same way and makes only the others again
    (TOTK_SEED_VERIFY, seed_verified; user 2026-11-11: "如何让 快速更新 不要这么容易失效"). Until then the source of the
    request functions counted, and removing a fallback for starting the translator (4b308d25) made all 42,891 translate again.

    The shader/bind-state key alone does not describe device depth mode. The old unversioned seed cache kept vertex
    shaders made with near=0 after the runtime was corrected to NVN's near=-w default.
    """
    inputs = dict(schema=3, chain=CHAIN, depth_mode=depth_mode, translator_sha256=sha256(translator))
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def run(cmd, log, env=None):
    with open(log, 'w') as out:
        r = subprocess.run([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, env=env)
    if r.returncode:
        raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}); see {log}')


def parallel_sha256(paths, jobs=None):
    """name -> sha256 of every path, hashed on all cores: the seed holds 250,000 files (3 GB); one thread took 8 minutes."""
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(jobs or min(16, (os.cpu_count() or 4) * 2)) as pool:
        return dict(zip((p.name for p in paths), pool.map(sha256, paths)))


def seed_files(cache, verified=None):
    """name -> path for the translations of the seed cache: each shader's index and the variants it lists; with verified
    (the variants the seed run checked against its runtime's requests or made), only those, and indexes -> the entries left
    of each shader's index (written into the seed in place of the cache's file)."""
    files, checks, indexes = {}, [], {}
    for index in sorted(cache.glob('*.variants')):
        if not VARIANTS.match(index.name):
            continue
        try:
            entries = json.loads(index.read_text())
        except (OSError, ValueError):
            continue
        kept = []
        for entry in entries if isinstance(entries, list) else []:
            name = entry.get('variant') if isinstance(entry, dict) else None
            if not isinstance(name, str) or not VARIANT.match(name):
                continue
            if name[:64] != index.stem:
                raise ValueError(f'seed index refers to another shader: {index}')
            if verified is not None and name not in verified:
                continue
            metal, refl = cache / f'{name}_b.metal', cache / f'{name}_b.json'
            if metal.is_file() and metal.stat().st_size and refl.is_file():
                checks.append((name, refl))
                files[metal.name], files[refl.name] = metal, refl
                kept.append(entry)
        if kept:
            files[index.name] = index
            if verified is not None and len(kept) != len(entries):
                indexes[index.name] = kept
    # The reflections are checked on all cores (42,845 JSON files; one thread read them for minutes).
    def check(item):
        name, refl = item
        reflection = json.loads(refl.read_text())
        if reflection.get('chain') != CHAIN or reflection.get('key') != name + '_b' or not reflection.get('entry'):
            raise ValueError(f'invalid seed reflection: {refl}')
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(min(16, (os.cpu_count() or 4) * 2)) as pool:
        for _ in pool.map(check, checks):
            pass
    return (files, indexes) if verified is not None else files


def corpus_inventory(path):
    """Check the actual records, not the seed tool's (formerly filtered) queue count."""
    inventory = {}
    with path.open('rb') as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:8] != CORPUS_MAGIC:
            raise ValueError('invalid shader corpus')
        count, = struct.unpack('<I', header[8:])
        for _ in range(count):
            st, cn, kn, ln = struct.unpack('<IIIH', stream.read(14))
            label = stream.read(ln).decode()
            control, code = stream.read(cn), stream.read(kn)
            if len(control) != cn or len(code) != kn:
                raise ValueError('truncated shader corpus')
            key = shader_key(control, code)
            if key in inventory:
                raise ValueError('duplicate shader in corpus: ' + key)
            inventory[key] = dict(stage=st, source=label)
        if stream.read(1) or not count:
            raise ValueError('empty corpus or trailing bytes')
    return inventory


def require_complete(inventory, keys, done, listed=None):
    missing = sorted(inventory.keys() - set(keys))
    if missing or not done or int(done.group(1)) != len(inventory) or int(done.group(2)) != len(inventory):
        raise ValueError(f'incomplete shader seed: {len(inventory) - len(missing)}/{len(inventory)} keys; '
                         f'{len(missing)} missing (including skipped stages)')
    if listed and listed.group(1) != listed.group(2):   # a listed variant only warms up: one that fails is translated at its draw, and fails the same way there
        print(f'[RED] the shader list: {listed.group(1)} of {listed.group(2)} bind states translated; the rest are left out of the warm-up', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--version', required=True, help='the game folder name (oot)')
    p.add_argument('--depth-mode', choices=('minus-w', 'zero'), required=True, help="the game's observed NVN device depth mode")
    p.add_argument('--binary', type=Path, required=True, help="the game's binary (build/totk-native-<version>)")
    p.add_argument('--list', type=Path, help="optional recorded bind states; OOT always extracts all resource shaders")
    p.add_argument('--jobs', type=int, default=4, help='bounded extraction/translation concurrency')
    p.add_argument('--output', type=Path, help='default build/shader-seed/<version>')
    p.add_argument('--logs', type=Path, default=Path('logs'))
    a = p.parse_args()
    if not 1 <= a.jobs <= 32:
        p.error('--jobs must be between 1 and 32')
    a.binary = a.binary.resolve()
    v = a.version
    out = a.output or Path(f'build/shader-seed/{v}')
    romfs, elf = Path(f'extracted/{v}/romfs'), Path(f'extracted/{v}/elf')
    corpus, cache = Path(f'build/ns2-corpus/{v}.bin'), Path(f'build/ns2-seed-cache/{v}')
    a.logs.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    romfs_sha = json.loads((romfs / 'index.json').read_text())['romfs_sha256']

    stamp = corpus.with_suffix('.json')   # the corpus is made again only for other game files
    want = {'romfs_sha256': romfs_sha, 'modules': {e.name: sha256(e) for e in sorted(elf.glob('*.elf'))},
            'tool': sha256(Path(__file__).resolve().parent / 'ns2_shader_corpus.py'), 'list': sha256(a.list) if a.list else None}
    if not corpus.is_file() or not stamp.is_file() or json.loads(stamp.read_text()) != want:
        corpus.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, Path(__file__).resolve().parent / 'ns2_shader_corpus.py', '--romfs', romfs,
             '--modules', *sorted(elf.glob('*.elf')), '--out', corpus, '--jobs', a.jobs,
             *(['--listed', a.list] if a.list else [])], a.logs / f'ns2-corpus-{v}.log')
        stamp.write_text(json.dumps(want))

    prebuilt = Path('prebuilt/macos-arm64/totk_xlate')
    context = translation_context(prebuilt, a.depth_mode)
    cache = cache / context
    cache.mkdir(parents=True, exist_ok=True)
    # A concurrent App package may replace the shared build/shader-service binary.
    # This preparation uses its own immutable copy, named by the translation context.
    native = cache / 'tool/build'
    service = native / 'shader-service/totk_xlate'
    if not service.is_file() or sha256(service) != sha256(prebuilt):
        service.parent.mkdir(parents=True, exist_ok=True)
        # A new file, never written over the old one: macOS keeps the code signature it checked for a file it ran, and a
        # program overwritten in place is then killed at launch (SIGKILL, Code Signature Invalid; 2026-11-03).
        service.unlink(missing_ok=True)
        shutil.copy2(prebuilt, service)
    log = a.logs / f'ns2-seed-{v}.log'
    sys.path.insert(0, str(Path(__file__).resolve().parent / 'dev'))
    from game_slot import GameSlot
    os.environ.setdefault('TOTK_GAME_SLOT_DIR', str(Path.home() / 'Library/Application Support/MacNativeGames/TOTK/game-slot'))
    env = {k: value for k, value in os.environ.items() if k not in ('TOTK_SHADER_XLATE', 'TOTK_SHADER_DEBUGMODE')}
    verified_list = (cache / 'verified.txt').resolve()   # what this run checked or made (runtime_shader_seed)
    verified_list.unlink(missing_ok=True)
    env.update(TOTK_SHADER_CACHE=str(cache.resolve()), TOTK_SEED_DEPTH_MODE=a.depth_mode,
               TOTK_NATIVE_DIR=str(native.resolve()), TOTK_SHADER_SEED='off', TOTK_RT_BG_WORKERS=str(a.jobs),
               TOTK_SHADER_SERVICE='native', TOTK_XLATE_SPIRV_CROSS='inproc',
               TOTK_SEED_VERIFY='1', TOTK_SEED_VERIFIED_OUT=str(verified_list))
    with GameSlot(owner='build', purpose=f'{v}: complete offline shader translation', expect_seconds=1200) as slot:
        with log.open('w') as output:
            result = slot.popen([str(a.binary), f'rewritten/{v}', 'seed-shaders', str(corpus),
                                 *([str(a.list)] if a.list else [])], env=env, stdout=output, stderr=subprocess.STDOUT).wait()
        if result:
            raise SystemExit(f'seed-shaders failed ({result}); see {log}')
    text = log.read_text(errors='replace')
    done = re.search(r'\[seed\] done in \d+ s: (\d+) of (\d+) shaders', text)
    listed = re.search(r"the shader list's bind states: (\d+) of (\d+) translated", text)
    if not done:
        raise SystemExit(f'the seed did not finish; see {log}')

    if not verified_list.is_file():
        raise SystemExit(f'the seed run wrote no {verified_list.name}; see {log}')
    checked = re.search(r'\[seed\] requests checked: (\d+) variants for this runtime, (\d+) cached ones kept, (\d+) made again', text)
    if checked:
        print(f'translations: {checked.group(2)} kept for this runtime\'s requests, {checked.group(3)} made again '
              f'(another request), {int(checked.group(1)) - int(checked.group(2))} made in all', flush=True)
    files, indexes = seed_files(cache, set(verified_list.read_text().split()))
    keys = sorted(n[:-len('.variants')] for n in files if n.endswith('.variants'))
    inventory = corpus_inventory(corpus)
    missing = {k: inventory[k] for k in sorted(inventory.keys() - set(keys))}
    (a.logs / f'ns2-seed-{v}-missing.json').write_text(json.dumps(missing, indent=2) + '\n')
    require_complete(inventory, keys, done, listed)
    shaders = sum(n.endswith('_b.metal') for n in files)
    staging = out.with_name(out.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for name, src in files.items():
        if name in indexes:   # only the variants this run checked or made
            (staging / name).write_text(json.dumps(indexes[name], sort_keys=True, separators=(',', ':'), ensure_ascii=False))
            continue
        os.link(src, staging / name) if src.stat().st_dev == staging.stat().st_dev else shutil.copy2(src, staging / name)
    scope = shader_key_scope(keys, v, romfs_sha)
    (staging / 'shader-keys.json').write_text(json.dumps(scope, separators=(',', ':')) + '\n')
    # Unlike the shaders, a complete render-pipeline description needs the game's bind state: the warm-up lists come from
    # the shared list's recorded rows (S / C / R / B / T), each variant named as this seed serves it (shader_list.py
    # resolve, as the TOTK packaging's resolve_lists). Without a list the file only says so (never invent descriptors).
    if a.list:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from shader_list import ShaderList, EXPORT, list_name
        lst = ShaderList.load(a.list)
        title, counts_t = lst.resolve(staging, 't')
        rest, counts_m = lst.resolve(staging, 'm', given=title)
        for name, rows, counts in (('pso_title.tsv', title, counts_t), ('pso_manifest.tsv', rest, counts_m)):
            (staging / name).write_text(EXPORT.format(list_name(a.list)) + ''.join('\t'.join(row) + '\n' for row in rows))
            print(f'{name}: {len(rows)} warm rows; left out {counts["other"]} of other versions, {counts["missing"]} the seed cannot build, '
                  f'{counts["renamed"]} renamed to the seed\'s variants', flush=True)
    else:
        (staging / 'pso_manifest.tsv').write_text('# NS2 startup shaders; render pipelines come from the shared list.\n')
    report = dict(schema=1, chain=CHAIN, version=v, game=v, source_romfs_sha256=romfs_sha,
                  translation_context_sha256=context,
                  nvn_depth_mode=a.depth_mode,
                  runtime_sha256=sha256(a.binary), translator_sha256=sha256(prebuilt),
                  shader_list_sha256=sha256(a.list) if a.list else None,
                  corpus_sha256=sha256(corpus), corpus_inputs=want, all_default=True,
                  stages={str(st): sum(x['stage'] == st for x in inventory.values()) for st in range(6)},
                  preparation_sha256=sha256(Path(__file__).resolve()), shaders=shaders,
                  corpus_shaders=int(done.group(2)), translated_default=int(done.group(1)),
                  listed_variants=int(listed.group(1)) if listed else 0, listed_asked=int(listed.group(2)) if listed else 0,
                  seconds=round(time.time() - t0, 1),
                  files=parallel_sha256(sorted(staging.iterdir())))
    (staging / 'seed-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    # Publish first; deleting tens of thousands of old files on an external APFS volume takes minutes. The old seed
    # is entirely generated data and no longer read once its replacement is published.
    old = out.with_name(f'.{out.name}.old-{os.getpid()}-{time.time_ns()}')
    if out.exists(): out.rename(old)
    try:
        staging.rename(out)
    except OSError:
        if old.exists(): old.rename(out)
        raise
    if old.exists(): subprocess.Popen(['/bin/rm', '-rf', str(old)], start_new_session=True)
    record_verified(out, report['files'])
    print(json.dumps({k: report[k] for k in ('version', 'shaders', 'corpus_shaders', 'translated_default',
                                             'listed_variants', 'listed_asked', 'seconds')}))


if __name__ == '__main__':
    main()
