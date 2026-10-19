#!/usr/bin/env python3
"""Add locator rows (L) to the shared shader list for listed shaders that are not BNSH files.

agl keeps many of the game's shaders in .sharcb packages (Lib/agl, Shader/ApplicationPackage, Lib/Terrain, Lib/gsys) and
the game module holds a few; prepare_shader_seed.py finds BNSH files only. A translation cache holds each shader's code
as the game handed it over (<variant>.code), so its code is searched for in those places, its NVN control section found
next to it, and control plus code checked against the key. The row records only where they are: no game data.

  locate_shaders.py --romfs extracted/<version>/romfs --modules rewritten/<version> <translation cache folder>...
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


def packages(romfs):
    """Every agl shader package (.sharcb, magic BAHS) in the RomFS as (source path, decompressed bytes)."""
    import compression.zstd as zstd
    index = json.loads((romfs/'index.json').read_text())
    with (romfs/'romfs.bin').open('rb') as stream:
        def raw(row):
            stream.seek(row['offset']); return stream.read(row['size'])
        rows = {r['path']: r for r in index['files']}
        dictionaries = {}
        for data in seed.sarc(zstd.decompress(raw(rows['Pack/ZsDic.pack.zs']))).values():
            d = zstd.ZstdDict(data); dictionaries[d.dict_id] = d
        def decode(data):
            if data[:4] != seed.ZSTD:
                return data
            info = zstd.get_frame_info(data)
            return zstd.decompress(data, zstd_dict=dictionaries[info.dictionary_id] if info.dictionary_id else None)
        def visit(source, data, depth):
            if data[:4] == b'BAHS':
                yield source, data
            elif data[:4] == b'SARC' and depth < 4:
                for member, child in seed.sarc(data).items():
                    yield from visit(source+'/'+member, decode(child), depth+1)
        for name, row in sorted(rows.items()):
            if name.endswith('.zs') or name.endswith('.sharcb'):
                yield from visit(name, decode(raw(row)), 0)


def locate(blobs, targets):
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
                    if (control >= 0 and struct.unpack_from('<I', data, control+0x714)[0] == STAGES[stage] and
                            hashlib.sha256(data[control:control+0x880]+data[start:start+length]).hexdigest() == key):
                        found[key] = (source, control, start, length)
                        break
                    q += 1
                at = data.find(body, at+1)
    return found


def main(argv):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--romfs', type=Path, required=True)
    p.add_argument('--modules', type=Path, help='rewritten/<version>: the game module images')
    p.add_argument('--list', type=Path, default=seed.LIST)
    p.add_argument('caches', type=Path, nargs='+', help='translation cache folders (their <variant>.code files)')
    a = p.parse_args(argv)
    shared = ShaderList.load(a.list)
    _, bnsh, _, _ = seed.inventory(a.romfs.resolve(), a.modules)
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
    blobs = list(packages(a.romfs.resolve()))
    for module in sorted(a.modules.glob('*.patched.bin')) if a.modules else []:
        blobs.append(('module:'+module.name[:-len('.patched.bin')], module.read_bytes()))
    found = locate(blobs, targets)
    shared.locators |= {(key, *where) for key, where in found.items()}
    shared.save(a.list)
    print(f'located {len(found)} of them; {shared.counts()}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
