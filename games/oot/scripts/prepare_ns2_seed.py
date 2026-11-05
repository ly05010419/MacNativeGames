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
from prepare_shader_seed import CHAIN, shader_key_scope   # noqa: E402

VARIANTS = re.compile(r'^[0-9a-f]{64}\.variants$')
VARIANT = re.compile(r'^[0-9a-f]{64}-[0-9a-f]{12}$')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def translation_context(binary, translator, depth_mode):
    """A cache belongs to the executable making requests and the translator answering them.

    The shader/bind-state key alone does not describe device depth mode. The old unversioned seed cache kept vertex
    shaders made with near=0 after the runtime was corrected to NVN's near=-w default.
    """
    inputs = dict(schema=1, chain=CHAIN, depth_mode=depth_mode,
                  runtime_sha256=sha256(binary), translator_sha256=sha256(translator))
    return hashlib.sha256(json.dumps(inputs, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def run(cmd, log, env=None):
    with open(log, 'w') as out:
        r = subprocess.run([str(c) for c in cmd], stdout=out, stderr=subprocess.STDOUT, env=env)
    if r.returncode:
        raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}); see {log}')


def seed_files(cache):
    """name -> path for the translations of the seed cache: each shader's index and the variants it lists."""
    files = {}
    for index in sorted(cache.glob('*.variants')):
        if not VARIANTS.match(index.name):
            continue
        try:
            entries = json.loads(index.read_text())
        except (OSError, ValueError):
            continue
        kept = False
        for entry in entries if isinstance(entries, list) else []:
            name = entry.get('variant') if isinstance(entry, dict) else None
            if not isinstance(name, str) or not VARIANT.match(name):
                continue
            metal, refl = cache / f'{name}_b.metal', cache / f'{name}_b.json'
            if metal.is_file() and refl.is_file():
                files[metal.name], files[refl.name] = metal, refl
                kept = True
        if kept:
            files[index.name] = index
    return files


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

    stamp = corpus.with_suffix('.json')   # the corpus is made again only for other game files
    want = {'romfs_sha256': romfs_sha, 'modules': {e.name: sha256(e) for e in sorted(elf.glob('*.elf'))},
            'tool': sha256(Path(__file__).resolve().parent / 'ns2_shader_corpus.py'), 'list': sha256(a.list)}
    if not corpus.is_file() or not stamp.is_file() or json.loads(stamp.read_text()) != want:
        corpus.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, Path(__file__).resolve().parent / 'ns2_shader_corpus.py', '--romfs', romfs,
             '--modules', *sorted(elf.glob('*.elf')), '--out', corpus, '--listed', a.list], a.logs / f'ns2-corpus-{v}.log')
        stamp.write_text(json.dumps(want))

    service = Path('build/shader-service/totk_xlate')   # where the runtime looks for the translator (totk_shader_rt.m)
    prebuilt = Path('prebuilt/macos-arm64/totk_xlate')
    if not service.is_file() or sha256(service) != sha256(prebuilt):
        service.parent.mkdir(parents=True, exist_ok=True)
        # A new file, never written over the old one: macOS keeps the code signature it checked for a file it ran, and a
        # program overwritten in place is then killed at launch (SIGKILL, Code Signature Invalid; 2026-11-03).
        service.unlink(missing_ok=True)
        shutil.copy2(prebuilt, service)
    context = translation_context(a.binary, prebuilt, a.depth_mode)
    cache = cache / context
    cache.mkdir(parents=True, exist_ok=True)
    log = a.logs / f'ns2-seed-{v}.log'
    run([a.binary, f'rewritten/{v}', 'seed-shaders', corpus, a.list], log,
        env=dict(os.environ, TOTK_SHADER_CACHE=str(cache.resolve()), TOTK_SEED_DEPTH_MODE=a.depth_mode,
                 TOTK_RT_BG_WORKERS=str(os.cpu_count() or 8)))
    text = log.read_text(errors='replace')
    done = re.search(r'\[seed\] done in \d+ s: (\d+) of (\d+) shaders', text)
    listed = re.search(r"the shader list's bind states: (\d+) of (\d+) translated", text)
    if not done:
        raise SystemExit(f'the seed did not finish; see {log}')

    files = seed_files(cache)
    keys = sorted(n[:-len('.variants')] for n in files if n.endswith('.variants'))
    shaders = sum(n.endswith('_b.metal') for n in files)
    staging = out.with_name(out.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    for name, src in files.items():
        os.link(src, staging / name) if src.stat().st_dev == staging.stat().st_dev else shutil.copy2(src, staging / name)
    scope = shader_key_scope(keys, v, romfs_sha)
    (staging / 'shader-keys.json').write_text(json.dumps(scope, separators=(',', ':')) + '\n')
    # Unlike the shaders, a complete render-pipeline description needs the game's bind state. NS2 starts without
    # recorded pipeline rows; the shared list adds real rows as the game is played (never invent descriptors).
    (staging / 'pso_manifest.tsv').write_text('# NS2 startup shaders; render pipelines come from the shared list.\n')
    report = dict(schema=1, chain=CHAIN, version=v, game=v, source_romfs_sha256=romfs_sha,
                  translation_context_sha256=context,
                  nvn_depth_mode=a.depth_mode,
                  translator_sha256=sha256(prebuilt), shader_list_sha256=sha256(a.list),
                  preparation_sha256=sha256(Path(__file__).resolve()), shaders=shaders,
                  corpus_shaders=int(done.group(2)), translated_default=int(done.group(1)),
                  listed_variants=int(listed.group(1)) if listed else 0, listed_asked=int(listed.group(2)) if listed else 0,
                  seconds=round(time.time() - t0, 1),
                  files={p.name: sha256(p) for p in sorted(staging.iterdir())})
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
    print(json.dumps({k: report[k] for k in ('version', 'shaders', 'corpus_shaders', 'translated_default',
                                             'listed_variants', 'listed_asked', 'seconds')}))


if __name__ == '__main__':
    main()
