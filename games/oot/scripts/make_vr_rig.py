#!/usr/bin/env python3
"""Link's two models, decompressed from the player's own RomFS, for the VR first-person body (zeldaVR/runtime/oot_bfres.h).

The runtime has no zstd, the packaging Python has it in its standard library, so the App carries the two models as plain
files next to its other game data. Generated game content stays outside the checkout.
"""
import argparse
import compression.zstd as zstd
import json
import os
from pathlib import Path
import struct

MODELS = {'Player_Child': 'Model/Player_Child.Player_Child.bfres.zs', 'Player_Adult': 'Model/Player_Adult.Player_Adult.bfres.zs'}
DICTIONARIES = 'Pack/ZsDic.pack.zs'


def sarc_members(data):
    """name -> (start, end) of every member of a SARC archive."""
    if data[:4] != b'SARC' or data[6:8] != b'\xff\xfe':
        raise ValueError('the dictionary pack is not a little-endian SARC')
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    if size != len(data) or data[header:header + 4] != b'SFAT':
        raise ValueError('the dictionary pack is damaged')
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16
    if data[names:names + 4] != b'SFNT':
        raise ValueError('the dictionary pack has no name table')
    names += 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        at = names + (attribute & 0xffffff) * 4
        out[data[at:data.index(0, at, start)].decode()] = (start + lo, start + hi)
    return out


def write_if_changed(path, data):
    if path.is_file() and path.stat().st_size == len(data) and path.read_bytes() == data:
        return False
    temporary = path.with_name('.' + path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


def generate(romfs, output):
    romfs, output = Path(romfs), Path(output)
    repo = Path(__file__).resolve().parents[3]
    if (repo / '.git').exists() and (output.resolve() == repo or repo in output.resolve().parents):
        raise ValueError('generated VR models must remain outside the source checkout')
    files = {f['path']: f for f in json.loads((romfs / 'index.json').read_text())['files']}
    with (romfs / 'romfs.bin').open('rb') as image:
        def read(name):
            entry = files[name]
            image.seek(entry['offset'])
            raw = image.read(entry['size'])
            if len(raw) != entry['size']:
                raise ValueError('truncated RomFS entry: ' + name)
            return raw
        pack = zstd.decompress(read(DICTIONARIES))
        dictionaries = {}
        for name, (lo, hi) in sarc_members(pack).items():
            if name.endswith('.zsdic'):
                dictionary = zstd.ZstdDict(pack[lo:hi])
                dictionaries[dictionary.dict_id] = dictionary
        output.mkdir(parents=True, exist_ok=True)
        written = 0
        for name, path in MODELS.items():
            raw = read(path)
            wanted = zstd.get_frame_info(raw).dictionary_id
            if wanted not in dictionaries:
                raise ValueError(f'{path}: its compression dictionary {wanted} is not in the pack')
            model = zstd.decompress(raw, zstd_dict=dictionaries[wanted])
            if model[:4] != b'FRES':
                raise ValueError(path + ' is not a FRES model')
            written += write_if_changed(output / f'{name}.bfres', model)
    return written


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    print(f'VR models: {generate(args.romfs, args.output)} written, {args.output}')


if __name__ == '__main__':
    main()
