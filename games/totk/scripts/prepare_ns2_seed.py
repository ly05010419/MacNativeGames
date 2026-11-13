#!/usr/bin/env python3
"""Prepare a Switch 2 game's shader seed for its app (build_app.py; docs/ns2-port.md): the same folder Tears of the
Kingdom's prepare_shader_seed.py writes (build/shader-seed/<version>: *.variants, *_b.metal, *_b.json, shader-keys.json,
seed-manifest.json), so the app's shader steps take it as they are (step_shader_libs, shader_bundle, resolve_lists,
mac_pipelines).

A Switch 2 game's shaders are not BNSH files in Shader/ packages: scripts/ns2_shader_corpus.py finds every one the game
ships (its modules, LINKDATA, and of its RDB data files' 130,000 shaders those the shared list names: the ones the game
was seen to load) and the game binary's seed-shaders mode translates each through the
runtime itself (totk_rt_variant), so keys and variant names are the runtime's own: the default bind state for every
shader, and every bind state the game's shared shader list (games/<game>/assets/shader_list.tsv) names, with the answers
a build gives that row. The translations go into a cache kept in the work folder (build/ns2-seed-cache/<version>), so
a rebuild translates only what changed; the seed is that cache's translations, checked and listed in its manifest.

The output is game-derived data: it belongs in the work folder, never in the repository.

  prepare_ns2_seed.py --version pkp --depth-mode zero --binary build/totk-native-pkp --list ../pokopia/assets/shader_list.tsv
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from prepare_shader_seed import CHAIN, shader_key_scope, record_verified   # noqa: E402
from ns2_shader_corpus import KEY_RULE, list_key_rule   # noqa: E402  (the key rule the seed's keys are made with; update_runtime.py checks it)

VARIANTS = re.compile(r'^[0-9a-f]{64}\.variants$')
VARIANT = re.compile(r'^[0-9a-f]{64}-[0-9a-f]{12}$')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def translation_context(translator, depth_mode, spirv_cross=None):
    """The cache folder: what decides every translation at once, the chain, the device depth mode, the translator and the
    spirv-cross it runs (build/spirv-cross/spirv-cross; the runtime passes its path, totk_shader_rt.m service_start). Not the
    runtime that makes the requests: each translation keeps the request it was made from (totk_shader_rt.m variant_request,
    its index entry's "request"), and a seed run of another runtime keeps those it would ask for the same way and makes only
    the others again (TOTK_SEED_VERIFY, seed_verified; user 2026-11-11: "如何让 快速更新 不要这么容易失效"). Until then the
    whole runtime binary counted, and every runtime change translated every shader again.

    The shader/bind-state key alone does not describe device depth mode. The old unversioned seed cache kept vertex
    shaders made with near=0 after the runtime was corrected to NVN's near=-w default.
    """
    inputs = dict(schema=2, chain=CHAIN, depth_mode=depth_mode, translator_sha256=sha256(translator),
                  spirv_cross_sha256=sha256(spirv_cross) if spirv_cross and Path(spirv_cross).is_file() else None)
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def run(cmd, log, env=None):
    with open(log, 'w') as out:
        r = subprocess.run([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, env=env)
    if r.returncode:
        raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}); see {log}')


def publish_digests(paths, seed, hash_all):
    """name -> SHA-256 of the seed about to be published, and how many files were read for it. A file that is still the
    cache file the last published seed had (same device, inode, size and modification time in that seed's .verified.json,
    record_verified) keeps the digest recorded then; every other one is read (hash_all). The change time is not compared:
    linking a cache file into the new seed changes its inode's. A translation made again is a new file, and an index this
    run writes is new too (user 2026-11-11 "极速更新": hashing all 250,000 files took 3 minutes of every update)."""
    known = {}
    try:
        record = json.loads(seed.with_name(seed.name + '.verified.json').read_text())
        if record.get('schema') == 1 and isinstance(record.get('files'), dict):
            known = record['files']
    except (OSError, ValueError):
        pass
    out, todo = {}, []
    for path in paths:
        entry = known.get(path.name)
        try:
            st = path.stat()
        except OSError:
            todo.append(path)
            continue
        if isinstance(entry, list) and len(entry) == 6 and entry[:4] == [st.st_dev, st.st_ino, st.st_size, st.st_mtime_ns]:
            out[path.name] = entry[5]
        else:
            todo.append(path)
    out.update(hash_all(todo))
    return out, len(todo)



def parallel_sha256(paths):
    """name -> sha256 of every path, on all cores."""
    import concurrent.futures
    with concurrent.futures.ThreadPoolExecutor(min(16, (os.cpu_count() or 4) * 2)) as pool:
        return dict(zip((p.name for p in paths), pool.map(sha256, paths)))


GENERATED = {'shader-keys.json', 'pso_title.tsv', 'pso_manifest.tsv'}   # written by the publish itself, from the inputs
# Every shader the game ships (the corpus finds them all and keeps only those the list names), as shader_key_scope writes
# it: which shaders are this game's, whatever this seed translated (shader-keys.json names only those). The shader merge
# (games/totk/scripts/collect_shaders.py) reads it, so a row naming a shader outside it is never this game's (user
# 2026-11-12: every game's shaders apart; Pokopia's seed named 8,291 of its 131,090 shaders). Not in the manifest: the
# runtime takes the manifest's files only by its own names (totk_shader_seed.h) and never reads this one.
IDENTITY = 'game-shader-keys.json'


def write_identity(folder, identity):
    """The game's shader keys into folder/IDENTITY, written only when they changed (the shader data bundle is signed again
    when a file of the seed folder changes, build_app.shader_bundle)."""
    path, text = folder / IDENTITY, json.dumps(identity, separators=(',', ':')) + '\n'
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)


def seed_unchanged(out, files, inputs):
    """Whether the published seed already is this one: made from the same inputs (translator, chain, depth mode, game
    files, shader list, corpus and the scripts that write it), and every translation and index file is the very file it has
    (same device and inode: a translation made again, or an index this run writes, is another file). Then it is kept as it
    is, without new links, digests or manifest, so the shader data bundle made from it is kept too (user 2026-11-11 "极速
    更新": publishing an unchanged seed took 3 minutes, and its new manifest had the bundle signed again, 3 more)."""
    try:
        old = json.loads((out / 'seed-manifest.json').read_text())
        if old.get('inputs') != inputs:
            return False
        names = set(old.get('files', {}))
        if not set(files) <= names or not names - set(files) <= GENERATED:
            return False
        for name, src in files.items():
            a, b = (out / name).stat(), src.stat()
            if (a.st_dev, a.st_ino) != (b.st_dev, b.st_ino):
                return False
        return True
    except (OSError, ValueError, AttributeError):
        return False


def seed_files(cache, verified=None):
    """name -> path for the translations of the seed cache: each shader's index and the variants it lists; with verified
    (the variants the seed run checked against its runtime's requests or made), only those, and indexes -> the entries left
    of each shader's index (written into the seed in place of the cache's file)."""
    files, indexes = {}, {}
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
            if verified is not None and name not in verified:
                continue
            metal, refl = cache / f'{name}_b.metal', cache / f'{name}_b.json'
            if metal.is_file() and refl.is_file():
                files[metal.name], files[refl.name] = metal, refl
                kept.append(entry)
        if kept:
            files[index.name] = index
            if verified is not None and len(kept) != len(entries):
                indexes[index.name] = kept
    return (files, indexes) if verified is not None else files


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--version', required=True, help='the game folder name (pkp)')
    p.add_argument('--depth-mode', choices=('minus-w', 'zero'), required=True, help="the game's observed NVN device depth mode")
    p.add_argument('--binary', type=Path, required=True, help="the game's binary (build/totk-native-<version>)")
    p.add_argument('--list', type=Path, required=True, help="the game's shared shader list")
    p.add_argument('--output', type=Path, help='default build/shader-seed/<version>')
    p.add_argument('--logs', type=Path, default=Path('logs'))
    a = p.parse_args()
    v = a.version
    out = a.output or Path(f'build/shader-seed/{v}')
    romfs, elf = Path(f'extracted/{v}/romfs'), Path(f'extracted/{v}/elf')
    corpus, cache = Path(f'build/ns2-corpus/{v}.bin'), Path(f'build/ns2-seed-cache/{v}')
    a.logs.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    romfs_sha = json.loads((romfs / 'index.json').read_text())['romfs_sha256']
    if list_key_rule(a.list) != KEY_RULE:   # its rows would name no shader of this seed
        raise SystemExit(f'{a.list}: names its shaders by key rule {list_key_rule(a.list) or "(none said)"}, the seed by {KEY_RULE}: '
                         'move it with ns2_rekey.py --apply first')

    stamp = corpus.with_suffix('.json')   # the corpus is made again only for other game files
    all_keys = corpus.with_suffix('.keys.json')   # every shader the game ships (IDENTITY)
    want = {'romfs_sha256': romfs_sha, 'modules': {e.name: sha256(e) for e in sorted(elf.glob('*.elf'))},
            'tool': sha256(Path(__file__).resolve().parent / 'ns2_shader_corpus.py'), 'list': sha256(a.list)}
    if not corpus.is_file() or not all_keys.is_file() or not stamp.is_file() or json.loads(stamp.read_text()) != want:
        corpus.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, Path(__file__).resolve().parent / 'ns2_shader_corpus.py', '--romfs', romfs,
             '--modules', *sorted(elf.glob('*.elf')), '--out', corpus, '--listed', a.list, '--all-keys', all_keys],
            a.logs / f'ns2-corpus-{v}.log')
        stamp.write_text(json.dumps(want))

    service = Path('build/shader-service/totk_xlate')   # where the runtime looks for the translator (totk_shader_rt.m)
    prebuilt = Path('prebuilt/macos-arm64/totk_xlate')
    if not service.is_file() or sha256(service) != sha256(prebuilt):
        service.parent.mkdir(parents=True, exist_ok=True)
        # A new file, never written over the old one: macOS keeps the code signature it checked for a file it ran, and a
        # program overwritten in place is then killed at launch (SIGKILL, Code Signature Invalid; 2026-11-03).
        service.unlink(missing_ok=True)
        shutil.copy2(prebuilt, service)
    spirv_cross = Path('build/spirv-cross/spirv-cross')   # the one the runtime passes to the translator
    context = translation_context(prebuilt, a.depth_mode, spirv_cross)
    cache = cache / context
    cache.mkdir(parents=True, exist_ok=True)
    log = a.logs / f'ns2-seed-{v}.log'
    verified_list = (cache / 'verified.txt').resolve()   # what this run checked or made (runtime_shader_seed)
    verified_list.unlink(missing_ok=True)
    run([a.binary, f'rewritten/{v}', 'seed-shaders', corpus, a.list], log,
        env=dict(os.environ, TOTK_SHADER_CACHE=str(cache.resolve()), TOTK_SEED_DEPTH_MODE=a.depth_mode,
                 TOTK_RT_BG_WORKERS=str(os.cpu_count() or 8), TOTK_SEED_VERIFY='1', TOTK_SEED_VERIFIED_OUT=str(verified_list)))
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
    shaders = sum(n.endswith('_b.metal') for n in files)
    here = Path(__file__).resolve().parent
    inputs = dict(chain=CHAIN, shader_key_rule=KEY_RULE, version=v, source_romfs_sha256=romfs_sha, translation_context_sha256=context,
                  nvn_depth_mode=a.depth_mode, translator_sha256=sha256(prebuilt), shader_list_sha256=sha256(a.list),
                  corpus_sha256=sha256(corpus), scripts_sha256={n: sha256(here / n) for n in ('prepare_ns2_seed.py', 'prepare_shader_seed.py')})
    identity = shader_key_scope(json.loads(all_keys.read_text()), v, romfs_sha)
    if not set(keys) <= set(identity['keys']):   # the seed translated only shaders the corpus found
        raise SystemExit(f'{len(set(keys) - set(identity["keys"]))} translated shaders are not among the game\'s {len(identity["keys"])}')
    if not indexes and seed_unchanged(out, files, inputs):
        write_identity(out, identity)
        print(f'seed unchanged: its {len(files)} translation files are the ones published, from the same inputs; '
              'kept as it is (no new links, digests or manifest)', flush=True)
        return
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
    # Unlike the shaders, a complete render-pipeline description needs the game's bind state. NS2 starts without
    # recorded pipeline rows; the shared list adds real rows as the game is played (never invent descriptors).
    (staging / 'pso_manifest.tsv').write_text('# NS2 startup shaders; render pipelines come from the shared list.\n')
    report = dict(schema=1, chain=CHAIN, shader_key_rule=KEY_RULE, version=v, game=v, source_romfs_sha256=romfs_sha,
                  translation_context_sha256=context,
                  nvn_depth_mode=a.depth_mode,
                  translator_sha256=sha256(prebuilt), shader_list_sha256=sha256(a.list),
                  preparation_sha256=sha256(Path(__file__).resolve()), shaders=shaders,
                  corpus_shaders=int(done.group(2)), translated_default=int(done.group(1)),
                  listed_variants=int(listed.group(1)) if listed else 0, listed_asked=int(listed.group(2)) if listed else 0,
                  seconds=round(time.time() - t0, 1), inputs=inputs, files={})
    report['files'], read = publish_digests(sorted(staging.iterdir()), out, parallel_sha256)
    print(f'seed files: {len(report["files"])}, {read} read again (the rest kept the digest of the last seed)', flush=True)
    (staging / 'seed-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    write_identity(staging, identity)   # after the manifest's digests: never one of its files
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
    record_verified(out, report['files'])   # the next publish keeps the digests of the files that stay (publish_digests)
    print(json.dumps({k: report[k] for k in ('version', 'shaders', 'corpus_shaders', 'translated_default',
                                             'listed_variants', 'listed_asked', 'seconds')}))


if __name__ == '__main__':
    main()
