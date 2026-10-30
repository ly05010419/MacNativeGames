#!/usr/bin/env python3
"""Add locator rows (L) to the shared shader list for listed shaders that are not BNSH files.

agl keeps many of the game's shaders in .sharcb packages (Lib/agl, Shader/ApplicationPackage, Lib/Terrain, Lib/gsys) and
the game module holds a few; prepare_shader_seed.py finds BNSH files only. A translation cache holds each shader's code
as the game handed it over (<variant>.code), so its code is searched for in those places, its NVN control section found
next to it, and control plus code checked against the key. The row records only where they are: no game data.

  locate_shaders.py --romfs extracted/<version>/romfs --modules rewritten/<version> <translation cache folder>...
Breath of the Wild (--game botw): its list, its Yaz0 packages, its key (the control section's own length, agl's as
the game rewrites it; prepare_shader_seed.py control_key_len / bound_control).
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_shader_seed as seed   # noqa: E402
from shader_list import ShaderList   # noqa: E402

STAGES = {'vs': 0, 'ps': 1, 'cs': 5}


def packages(romfs, game='totk'):
    """Every agl shader package (.sharcb, magic BAHS) in the RomFS as (source path, decompressed bytes)."""
    index = json.loads((romfs/'index.json').read_text())
    with (romfs/'romfs.bin').open('rb') as stream:
        rows = {r['path']: r for r in index['files']}
        def raw(name):
            row = rows[name]; stream.seek(row['offset']); return stream.read(row['size'])
        decode = seed.decoder(romfs, raw, game)
        folders = seed.GAMES[game]['folders']
        def visit(source, data, depth):
            if data[:4] == b'BAHS':
                yield source, data
            elif data[:4] == b'SARC' and depth < 4:
                for member, child in seed.sarc(data).items():
                    yield from visit(source+'/'+member, decode(child), depth+1)
        for name in sorted(rows):
            if folders and not name.startswith(folders):
                continue
            if name.endswith('.zs') or name.endswith('.sharcb') or (game == 'botw' and name.endswith(('.pack', '.ssarc', '.sarc'))):
                yield from visit(name, decode(raw(name)), 0)


def locate(blobs, targets, game='totk'):
    """targets: key -> (stage, code as the game handed it over). Returns {key: (source, control, code, length)}."""
    found = {}
    for source, data in blobs:
        for key, (stage, body) in targets.items():
            if key in found:
                continue
            skip = 0 if stage == 'cs' else 0x30   # a cache's .code leaves out the graphics code's 0x30-byte NVN header
            at = data.find(body)
            while at >= 0 and key not in found:
                start, length = at - skip, len(body) + skip
                pattern, q = struct.pack('<I', length), 0
                while start >= 0 and (q := data.find(pattern, q)) >= 0:
                    control = q - 0x704
                    section = data[control:control+0x880] if control >= 0 else b''
                    section = section[:seed.control_key_len(section)] if len(section) >= 0x7cc else b''
                    if (section and struct.unpack_from('<I', section, 0x714)[0] == STAGES[stage] and any(
                            hashlib.sha256(bound+data[start:start+length]).hexdigest() == key
                            for bound in (section, seed.bound_control(section, game)))):
                        found[key] = (source, control, start, length)
                        break
                    q += 1
                at = data.find(body, at+1)
    return found


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', type=Path, required=True)
    p.add_argument('--modules', type=Path, help='rewritten/<version>: the game module images')
    p.add_argument('--list', type=Path, help="the shared shader list (default: the game's)")
    p.add_argument('--game', choices=sorted(seed.GAMES), default='totk', help='totk (default) or botw')
    p.add_argument('caches', type=Path, nargs='+', help='translation cache folders (their <variant>.code files)')
    a = p.parse_args(argv)
    a.list = a.list or seed.GAMES[a.game]['list']
    shared = ShaderList.load(a.list)
    _, bnsh, _, _ = seed.inventory(a.romfs.resolve(), a.modules, a.game)
    wanted = {key for key, _ in shared.variants} - set(bnsh) - {where[0] for where in shared.locators}
    targets = {}
    for folder in a.caches:
        for code in folder.glob('*.code'):
            key = code.name.split('-')[0]
            reflection = folder/(code.name[:-len('.code')]+'_b.json')
            if key in wanted and key not in targets and reflection.is_file():
                stage = json.loads(reflection.read_text()).get('stage')
                if stage in STAGES:
                    targets[key] = (stage, code.read_bytes())
    print(f'{len(wanted)} listed shaders are no BNSH file of this game; {len(targets)} of them have their code in a cache')
    blobs = list(packages(a.romfs.resolve(), a.game))
    for module in sorted(a.modules.glob('*.patched.bin')) if a.modules else []:
        blobs.append(('module:'+module.name[:-len('.patched.bin')], module.read_bytes()))
    found = locate(blobs, targets, a.game)
    shared.locators |= {(key, *where) for key, where in found.items()}
    shared.save(a.list)
    print(f'located {len(found)} of them; {shared.counts()}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
