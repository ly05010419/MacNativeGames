#!/usr/bin/env python3
"""Prepare the app's startup shader seed from translation caches this Mac already has (the work folder's, the app's).

The graphics layer imports <game>/build/shader-seed at start (gfx/nvn/totk_shader_seed.h): every file is checked against
seed-manifest.json's SHA-256, then cloned into the player's cache, so a fresh app does not translate shaders while
playing (Future Redeemed's opening asked for about 10,000 at once). TotK prepares its seed from the RomFS shader packages
(games/totk/scripts/prepare_shader_seed.py); Xenoblade Chronicles 3 keeps its shaders in its own archive, so this seed is
made from the variants this game already met in play on this Mac. The output contains game-derived data and belongs in
the work folder or the local app, never in the repository.

Taken: <key>.variants (the variant index: merged from every cache, rows kept only when their files are present),
<key>-<12 hex>_b.metal and _b.json (the translation and its reflection). No pipeline list (pso_manifest.tsv): the runtime
waits at start until the seed's list is built, which for every pipeline met in play took 35 s (2026-10-29); the
shaders themselves are still compiled in the background from the start, and pipelines are collected again in play.
Geometry-as-compute variants (-vsc-/-gsc-/-vpt-) are
not taken: the import's file name check does not admit them, and they translate in milliseconds.
usage: prepare_shader_seed.py --out <work>/build/shader-seed <cache dir> [<cache dir> ...]
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
CHAIN_H = HERE / 'third_party/d2r_gfx/gfx/nvn/dmsh_chain.h'
INDEX = re.compile(r'^([0-9a-f]{64})\.variants$')
SHADER = re.compile(r'^([0-9a-f]{64}-[0-9a-f]{12})_b\.(metal|json)$')


def chain():
    m = re.search(r'#define DMSH_CHAIN "([^"]+)"', CHAIN_H.read_text())
    if not m:
        sys.exit(f'no DMSH_CHAIN in {CHAIN_H}')
    return m.group(1)


def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('caches', type=Path, nargs='+', help='translation cache folders (rt-<chain> or an app data shader-cache)')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    name_chain = chain()
    shaders, indexes = {}, {}   # variant name -> {ext: path}; key -> rows
    for cache in a.caches:
        if not cache.is_dir():
            print(f'skipped (no folder): {cache}')
            continue
        for entry in os.scandir(cache):
            m = SHADER.match(entry.name)
            if m:
                shaders.setdefault(m.group(1), {}).setdefault(m.group(2), Path(entry.path))
                continue
            m = INDEX.match(entry.name)
            if m:
                rows = json.loads(Path(entry.path).read_text())
                if not isinstance(rows, list):
                    sys.exit(f'not a variant index: {entry.path}')
                indexes.setdefault(m.group(1), []).extend(rows)
    complete = {v for v, files in shaders.items() if files.keys() == {'metal', 'json'}}
    out = a.out
    staging = out.with_name(out.name + '.incoming')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    files = {}
    for key, rows in sorted(indexes.items()):
        merged, seen = [], set()
        for row in rows:
            v = row.get('variant') if isinstance(row, dict) else None
            if isinstance(v, str) and v in complete and v not in seen and isinstance(row.get('deps'), dict):
                merged.append(row)
                seen.add(v)
        if not merged:
            continue
        name = f'{key}.variants'
        (staging / name).write_text(json.dumps(merged, sort_keys=True, separators=(',', ':')))
        for v in seen:
            for ext in ('metal', 'json'):
                target = staging / f'{v}_b.{ext}'
                if not target.exists():
                    os.link(shaders[v][ext], target) if shaders[v][ext].stat().st_dev == staging.stat().st_dev else shutil.copyfile(shaders[v][ext], target)
    lines = []
    for p in sorted(staging.iterdir()):
        files[p.name] = sha256(p)
    count = sum(1 for n in files if n.endswith('_b.metal'))
    if not count:
        sys.exit('no complete shader variant in the given caches')
    manifest = dict(chain=name_chain, schema=1, shaders=count, files=files)
    (staging / 'seed-manifest.json').write_text(json.dumps(manifest, sort_keys=True, indent=1) + '\n')
    shutil.rmtree(out, ignore_errors=True)
    staging.rename(out)
    print(json.dumps(dict(out=str(out), chain=name_chain, shaders=count, indexes=sum(n.endswith('.variants') for n in files),
                          pipelines=len(lines), files=len(files))))


if __name__ == '__main__':
    main()
