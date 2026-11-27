#!/usr/bin/env python3
"""Make Diablo III's iOS shader seed from a Mac runtime shader cache.

An iPhone or iPad cannot run the shader translator (a separate process), so the app carries the translated shaders. The
Mac game translates each shader variant the first time it is drawn and keeps it in its cache
(~/Library/Application Support/MacNativeGames/D3/shader-cache/rt-<chain>); this copies the files the runtime reads from a
seed (<key>.variants, <key>-<variant>_b.metal / .json) and the render pipelines seen in play (pso_collected.tsv, warmed
from pso_list.tsv) into a new folder and writes its manifest. The cache is only read. Variants the Mac never drew are not
in it: the device skips those draws and records them (shader-list-contribution.txt) for tools/fill_shaders.py.

  python3 make_seed.py <cache folder> <output seed folder>
"""
import hashlib, json, re, shutil, sys
from pathlib import Path

NAME = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json))$')

def chain_of(cache):
    m = re.fullmatch(r'rt-(.+)', cache.name)
    if not m: raise SystemExit(f'{cache}: expected a folder named rt-<translation chain>')
    return m.group(1)

def main():
    if len(sys.argv) != 3: raise SystemExit(__doc__)
    cache, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if not cache.is_dir(): raise SystemExit(f'no shader cache at {cache}')
    if out == cache or cache in out.parents: raise SystemExit('the seed must not be written into the cache')
    chain = chain_of(cache)
    names = sorted(n.name for n in cache.iterdir() if n.is_file() and NAME.match(n.name))
    metal = {n for n in names if n.endswith('_b.metal')}
    # A variant is usable only with its reflection; an index names only variants that are present.
    keep = [n for n in names if not n.endswith('_b.metal') or n[:-6] + '.json' in names]
    keep = [n for n in keep if not n.endswith('_b.json') or n[:-5] + '.metal' in metal]
    variants = {n.split('-')[0] for n in keep if n.endswith('_b.metal')}
    keep = [n for n in keep if not n.endswith('.variants') or n[:-9] in variants]
    if not variants: raise SystemExit(f'{cache} holds no translated shaders: play the game on the Mac first')
    usable = {n for n in keep if n.endswith('_b.metal')}   # variants whose source and reflection both come along
    tmp = out.with_name(out.name + '.partial'); shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    files = {}
    for n in keep:
        data = (cache / n).read_bytes()
        if n.endswith('.variants'):   # a JSON list of {variant, deps}: only entries whose files come along
            entries = [e for e in json.loads(data) if isinstance(e, dict) and f"{e.get('variant')}_b.metal" in usable]
            data = json.dumps(entries, separators=(',', ':')).encode()
        (tmp / n).write_bytes(data); files[n] = hashlib.sha256(data).hexdigest()
    pso = cache / 'pso_collected.tsv'
    rows = [l for l in pso.read_text(errors='replace').splitlines() if l.startswith(('R\t', 'B\t'))] if pso.is_file() else []
    (tmp / 'pso_list.tsv').write_text(''.join(r + '\n' for r in rows))
    manifest = {'chain': chain, 'schema': 1, 'shaders': len(variants), 'source': 'mac-runtime-cache',
                'variants': sum(n.endswith('_b.metal') for n in keep), 'pipelines': len(rows), 'files': files}
    (tmp / 'seed-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    shutil.rmtree(out, ignore_errors=True); tmp.rename(out)
    print(json.dumps({k: manifest[k] for k in ('chain', 'shaders', 'variants', 'pipelines')}))

if __name__ == '__main__': main()
