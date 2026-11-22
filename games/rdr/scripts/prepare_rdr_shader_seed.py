#!/usr/bin/env python3
"""Prepare Red Dead Redemption's whole-game shader seed from the local, verified RomFS and game modules.

iOS cannot translate shaders while playing, so the iPhone / iPad app carries this seed (build/shader-seed, imported by
the shared totk_shader_seed_ports.h; games/rdr/ios/tools/build_ipa.py), with the Mac's own translations merged in
(build_port_ipa.py seed_with_mac_cache). Nothing of the Zelda line's seed tool is written anew: Super Mario Odyssey's
(games/odyssey/scripts/prepare_odyssey_shader_seed.py, on its copy of Tears of the Kingdom's prepare_shader_seed.py) is
loaded and run as Mario Kart 8 Deluxe's is (this runtime is Mario Kart's, the same translation chain, dmsh_chain.h v4, the
same shader keys: nvn_mtl.m prog_get). What is this game's:
- Discovery: RAGE keeps its NVN programs in its own records, not in BNSH files: the 128 effects of shaders.rpf (an RPF6
  archive of Zstandard frames) and main (scripts/rpf6.py). 3,413 shaders in the archive and 168 in main (1.0.4); every
  shader the Mac runtime bound to Armadillo is one of them.
- Depth: this game takes NVN's 0..1 depth (its NVN 55 branch; every vertex row of the Mac's cache has depth_minus_w
  false), so the translations are made with depth_minus_w false and the vertex rows say so, where Odyssey's and Mario
  Kart's say true.
- Rule variants: the vertex layouts the Mac app met while playing (each vertex variant's input types, taken from its
  cache's indexes, as Mario Kart's tool takes its rules), are translated for every vertex shader (through the tool's own
  listed bind states; a state an earlier variant of the shader serves is not translated again). The translator takes an
  unanswered input's type from its use, so a default translation reads inputs 1 and 2 of the skinned shaders as uint,
  where the game binds input 2 as float: 176 of the Mac's 672 variants (1.0.4, to Armadillo) are such layouts.
Output: an importable seed directory (default <work>/build/shader-seed). It contains game-derived data and stays in the
work folder, never in the repo.

  python scripts/prepare_rdr_shader_seed.py --work <work> [--cache <Mac cache rt-totk-native-v4-unverified>]
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rpf6  # noqa: E402

GAME = Path(__file__).resolve().parents[1]
ODYSSEY_TOOL = GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'
VERSION = '1.0.4'
ARCHIVE = 'shaders.rpf'
CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
MIN_SHADERS = 3000
STAGE_NUMBER = {'vs': 0, 'ps': 1, 'cs': 5}
ALLOWED = re.compile(r'^(?:[0-9a-f]{64}\.variants|[0-9a-f]{64}-[0-9a-f]{12}_b\.(?:metal|json|metallib)|pso_manifest\.tsv)$')
READER = hashlib.sha256(Path(rpf6.__file__).read_bytes() + Path(__file__).read_bytes()).hexdigest()


def inventory(romfs, modules=None, game='totk'):
    """Every RAGE program of shaders.rpf and the game modules -> (stage, control, code, source), all of them translated
    for the default bind state (the seed tool's inventory(), for this game's containers)."""
    index = json.loads((Path(romfs) / 'index.json').read_text())
    if index.get('status') != 'pass':
        raise ValueError('RomFS has not passed integrity verification')
    row = {r['path']: r for r in index['files']}.get(ARCHIVE)
    if not row:
        raise ValueError(f'no {ARCHIVE} in the RomFS: not Red Dead Redemption')
    with (Path(romfs) / 'romfs.bin').open('rb') as f:
        f.seek(row['offset'])
        raw = f.read(row['size'])
    if len(raw) != row['size']:
        raise ValueError(f'short read of {ARCHIVE}')
    shaders = {}
    effects = rpf6.effects(raw)
    for at, effect in effects:
        for key, stage, control, code in rpf6.programs(effect):
            shaders.setdefault(key, (stage, control, code, f'{ARCHIVE}@{at:x}'))
    archived = len(shaders)
    for module in sorted(Path(modules).glob('*.patched.bin')) if modules else []:
        for key, stage, control, code in rpf6.programs(module.read_bytes()):
            shaders.setdefault(key, (stage, control, code, 'module:' + module.name[:-len('.patched.bin')]))
    print(f'discovered {len(shaders)} unique shaders: {archived} in {len(effects)} effects of {ARCHIVE}, '
          f'{len(shaders) - archived} more in the game modules', flush=True)
    if len(shaders) < MIN_SHADERS:
        raise ValueError(f'only {len(shaders)} shaders found; at least {MIN_SHADERS} expected (another game version?)')
    return index, shaders, set(shaders), [dict(path=ARCHIVE, sha256=hashlib.sha256(raw).hexdigest())]


def translation_request(variant, stage, control, code, answers, code_path):
    """The seed tool's translation_request with this game's depth: NVN's 0..1 range (depth_minus_w false)."""
    c1, length = struct.unpack_from('<II', control, 0x700)
    code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                   depth_minus_w=False, **answers)
    if stage == 'cs':
        request['local'] = list(struct.unpack_from('<III', control, 0x748))
    return request


def load_seed_tool(work, rules_list):
    """Odyssey's seed tool (its copy of the Zelda line's), pointed at this work folder and game."""
    spec = importlib.util.spec_from_file_location('odyssey_seed', ODYSSEY_TOOL)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    tool.BUILD = work
    seed = tool.load_upstream()
    seed.GAMES['totk'].update(folders=None, min_shaders=MIN_SHADERS, list=rules_list, all_default=True)
    seed.inventory = inventory
    seed.translation_request = translation_request
    cached = seed.cached_inventory   # its saved inventory names this reader too: a change here reads the archive again
    seed.cached_inventory = lambda romfs, modules, game, folder, identity: cached(
        romfs, modules, game, folder, {**identity, 'rdr_reader_sha256': READER})
    return seed


def rules_from_cache(cache):
    """The bind states to translate besides the default: each distinct vertex layout (the input types) a vertex variant
    of the Mac app's cache was made for (their .variants rows' deps), e.g. {"attr": {"0": "float", "1": "uint", "2": "float"}}."""
    found = set()
    for index in sorted(Path(cache).glob('*.variants')) if cache and Path(cache).is_dir() else []:
        for row in json.loads(index.read_text()):
            refl = Path(cache) / f"{row['variant']}_b.json"
            if not refl.is_file() or json.loads(refl.read_text()).get('stage') != 'vs':
                continue
            attr = row.get('deps', {}).get('attr') or {}
            if attr:
                found.add(json.dumps({'attr': attr}, sort_keys=True, separators=(',', ':')))
    return sorted(found)


def write_rules(path, rules, shaders):
    """The rules as the seed tool's listed bind states (shader_list.py V rows) for every vertex shader."""
    rows = [f'V\t{key}\t{deps}' for key, (stage, *_) in sorted(shaders.items()) if stage == 'vs' for deps in rules]
    path.write_text('# Red Dead Redemption: the vertex layouts the Mac app met, for every vertex shader '
                    '(scripts/prepare_rdr_shader_seed.py)\n' + ''.join(r + '\n' for r in rows))
    return len(rows)


def finish(output, chain):
    """Vertex rows say depth_minus_w false; files the importer rejects (shader-keys.json) go; the manifest re-hashed."""
    vertex = 0
    for index in output.glob('*.variants'):
        rows = json.loads(index.read_text())
        for row in rows:
            if json.loads((output / f"{row['variant']}_b.json").read_text()).get('stage') == 'vs':
                vertex += 1
                row['depth_minus_w'] = False
        index.write_text(json.dumps(rows, separators=(',', ':')) + '\n')
    (output / 'shader-keys.json').unlink(missing_ok=True)
    manifest = json.loads((output / 'seed-manifest.json').read_text())
    if manifest.get('chain') != chain:
        raise SystemExit(f"seed chain {manifest.get('chain')} is not the runtime's {chain}")
    stray = [p.name for p in output.iterdir() if p.name != 'seed-manifest.json' and not ALLOWED.match(p.name)]
    if stray:
        raise SystemExit(f'unexpected files in the seed: {stray[:5]}')
    manifest['files'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(output.iterdir()) if ALLOWED.match(p.name)}
    (output / 'seed-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return vertex


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--work', type=Path, required=True, help='the work folder (README.md)')
    p.add_argument('--output', type=Path, help='default <work>/build/shader-seed')
    p.add_argument('--cache', type=Path, help=f'the Mac app\'s translations for the rules (default <work>/build/{CHAIN_CACHE})')
    p.add_argument('--no-rules', action='store_true', help='the default bind state of every shader only')
    a = p.parse_args()
    work = a.work.expanduser().resolve()
    output = (a.output or work / 'build/shader-seed').expanduser().resolve()
    cache = (a.cache or work / 'build' / CHAIN_CACHE).expanduser()
    romfs, modules = work / 'extracted/local/romfs', work / 'rewritten/local'
    started = time.monotonic()
    rules = [] if a.no_rules else rules_from_cache(cache)
    rules_list = output.with_name(output.name + '.rules.tsv')
    rules_list.parent.mkdir(parents=True, exist_ok=True)
    _, shaders, _, _ = inventory(romfs, modules)
    print(f'rules from the Mac app\'s translations ({cache}): {rules}; {write_rules(rules_list, rules, shaders)} listed bind states',
          flush=True)
    seed = load_seed_tool(work, rules_list)
    seed.prepare(romfs, output, work / 'build/shader-service/totk_xlate', work / 'build/spirv-cross/spirv-cross', VERSION,
                 shader_list=rules_list, modules=modules, game='totk')
    vertex = finish(output, seed.CHAIN)
    manifest = json.loads((output / 'seed-manifest.json').read_text())
    print(f'seed {output}: {manifest.get("shaders")} shader variants of {manifest.get("discovered")} shaders discovered '
          f'({vertex} vertex), {len(manifest.get("skipped", []))} skipped, {len(manifest["files"])} files; '
          f'{time.monotonic() - started:.0f} s', flush=True)


if __name__ == '__main__':
    os.environ.setdefault('PYTHONUNBUFFERED', '1')
    sys.exit(main())
