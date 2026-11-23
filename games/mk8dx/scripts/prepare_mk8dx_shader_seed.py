#!/usr/bin/env python3
"""Prepare Mario Kart 8 Deluxe's whole-game shader seed from the local, verified RomFS and game modules.

iOS cannot translate shaders while playing, so the iPhone / iPad app carries this seed (build/shader-seed, imported by
the shared totk_shader_seed_ports.h). Nothing is written anew: Super Mario Odyssey's seed tool
(games/odyssey/scripts/prepare_odyssey_shader_seed.py, on its copy of Tears of the Kingdom's prepare_shader_seed.py) is
run as it is, since this runtime is Odyssey's (the same translation chain, dmsh_chain.h v4, the same shader keys and
requests: nvn_mtl.m prog_get, totk_shader_rt.m totk_rt_variant). What is this game's:
- Coverage: every file of the RomFS is scanned (Odyssey's four shader folders are Odyssey's): the courses' and karts'
  models (.szs, Yaz0, and .bfres) carry their own BNSH programs, the effects (.ptcl) and the agl packages theirs.
- Vertex rows record depth_minus_w as Odyssey's do (this runtime's NVN 53 branch takes Odyssey's depth mode: every vertex
  variant of the Mac app's cache has it, build/shader-cache/rt-totk-native-v4-unverified).
- Rule variants (Odyssey's extend_with_rules): the non-default vertex input types the Mac app met while playing (a vertex
  input read as uint), taken from its cache's indexes rather than written by hand, are translated
  for every shader of the game that uses that slot, so a course the Mac never drove is served on the phone too.
  (A vertex input read as uint: 154 of the 562 vertex variants of the first races.)
Output: an importable seed directory (default <work>/build/shader-seed). It contains game-derived data and stays in the
work folder, never in the repo.

  python scripts/prepare_mk8dx_shader_seed.py --work <work> [--cache <Mac cache rt-totk-native-v4-unverified>]
"""
import argparse
import importlib.util
import json
import struct
import os
from pathlib import Path
import sys
import time

GAME = Path(__file__).resolve().parents[1]
ODYSSEY_TOOL = GAME.parent / 'odyssey/scripts/prepare_odyssey_shader_seed.py'
VERSION = '4.0.1'
CHAIN_CACHE = 'shader-cache/rt-totk-native-v4-unverified'   # third_party/d2r_gfx/gfx/nvn/dmsh_chain.h
# The bind-state answers a rule sets: vertex input types (Odyssey's extend_with_rules knows a shader's used texture and
# vertex input slots only). Texture kinds are '*' (any) in this game's rows, or compare slots, which the tool's own
# 'compare' jobs translate; the two integer colour outputs the Mac met come with its translations (build_port_ipa.py
# seed_with_mac_cache).
KINDS = ('attr',)
DEFAULTS = {'attr': 'float'}
MIN_SHADERS = 1000


def unnamed_sarc(sarc):
    """The tool's SARC reader, and for an archive whose members have no names (some UI layout archives, e.g.
    UI/cmn/boot/bt_TitleLoading_00.szs: every SFAT attribute 0, the members known by their name hash only), the members
    named '#<hash>' with the same bounds checks. Any other refusal stands."""
    def read(data):
        try:
            return sarc(data)
        except ValueError:
            endian = '<' if data[6:8] == b'\xff\xfe' else '>'
            header, size, start = struct.unpack_from(endian + 'HxxII', data, 4)
            if data[:4] != b'SARC' or size != len(data) or header < 20 or data[header:header + 4] != b'SFAT':
                raise
            count = struct.unpack_from(endian + 'H', data, header + 6)[0]
            entries = [struct.unpack_from(endian + '4I', data, header + 12 + i * 16) for i in range(count)]
            if any(attribute for _, attribute, _, _ in entries) or header + 12 + count * 16 > start:
                raise
            result = {}
            for name_hash, _, lo, hi in entries:
                if hi < lo or start + hi > len(data):
                    raise ValueError('invalid SARC member') from None
                result[f'#{name_hash:08x}'] = data[start + lo:start + hi]
            return result
    return read


def load_odyssey(work):
    """Odyssey's seed tool, pointed at this work folder and game (its module globals, as its own main() reads them)."""
    spec = importlib.util.spec_from_file_location('odyssey_seed', ODYSSEY_TOOL)
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    tool.BUILD = work
    seed = tool.load_upstream()
    seed.GAMES['totk'].update(folders=None, min_shaders=MIN_SHADERS, list=GAME / 'assets/shader_list.tsv', all_default=True)
    seed.sarc = unnamed_sarc(seed.sarc)
    return tool, seed


def rules_from_cache(cache):
    """{stage: [{kind: {slot: type}}]}: each non-default vertex input / colour output type the Mac app's translations were
    made for (their .variants rows' deps), one rule per stage with every such slot, as Odyssey's hand-written RULES."""
    found = {}
    for index in sorted(Path(cache).glob('*.variants')) if cache and Path(cache).is_dir() else []:
        for row in json.loads(index.read_text()):
            refl = Path(cache) / f"{row['variant']}_b.json"
            if not refl.is_file():
                continue
            stage = json.loads(refl.read_text()).get('stage')
            for kind in KINDS:
                for slot, kind_type in (row.get('deps', {}).get(kind) or {}).items():
                    if kind_type != DEFAULTS[kind]:
                        found.setdefault(stage, {}).setdefault(kind, {})[slot] = kind_type
    return {stage: [kinds] for stage, kinds in sorted(found.items())}


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
    tool, seed = load_odyssey(work)
    started = time.monotonic()
    seed.prepare(work / 'extracted/local/romfs', output, work / 'build/shader-service/totk_xlate',
                 work / 'build/spirv-cross/spirv-cross', VERSION, modules=work / 'rewritten/local', game='totk')
    tool.finish(output, seed.CHAIN)
    rules = {} if a.no_rules else rules_from_cache(cache)
    print(f'rules from the Mac app\'s translations ({cache}): {json.dumps(rules, sort_keys=True)}', flush=True)
    if rules:
        tool.RULES = rules
        tool.extend_with_rules(seed, output)
        tool.finish(output, seed.CHAIN)
    manifest = json.loads((output / 'seed-manifest.json').read_text())
    print(f'seed {output}: {manifest.get("shaders")} shaders with an index of {manifest.get("discovered")} discovered, '
          f'{len(manifest.get("skipped", []))} skipped, {len(manifest["files"])} files; {time.monotonic() - started:.0f} s',
          flush=True)


if __name__ == '__main__':
    os.environ.setdefault('PYTHONUNBUFFERED', '1')
    sys.exit(main())
