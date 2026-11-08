#!/usr/bin/env python3
"""Prepare live HUD-prompt fingerprints from the player's own built app.

The output contains dimensions and hashes, no game images. The renderer only
replaces an R8 texture whose pixels match one of these eight known HUD masks.
"""
import argparse
import compression.zstd as zstd
from pathlib import Path
import make_mod_manifest
from make_romfs_patches import sarc_members
from prompt_assets import hud_fingerprints


def prepare(app, version, output, romfs=None):
    romfs = romfs or app / f'Contents/Resources/totk/extracted/{version}/romfs'
    image_dir = romfs if (romfs / 'romfs.bin').is_file() else make_mod_manifest.app_image_dir(app, version)
    index = make_mod_manifest.read_index(romfs / 'runtime-index.bin')
    layout = next(p for p in index if p.startswith('UI/LayoutArchive/Common.') and p.endswith('.blarc.zs'))
    with (image_dir / 'romfs.bin').open('rb') as image:
        def read(path):
            offset, size = index[path]
            image.seek(offset)
            data = image.read(size)
            if len(data) != size:
                raise ValueError(f'truncated RomFS entry: {path}')
            return data
        pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
        dictionaries = {}
        for start, end in sarc_members(pack).values():
            dictionary = zstd.ZstdDict(pack[start:end])
            dictionaries[dictionary.dict_id] = dictionary
        raw = read(layout)
    info = zstd.get_frame_info(raw)
    archive = zstd.decompress(raw, zstd_dict=dictionaries[info.dictionary_id]) if info.dictionary_id else zstd.decompress(raw)
    start, end = sarc_members(archive)['timg/__Combined.bntx']
    rows = hud_fingerprints(archive[start:end])
    text = ''.join(f'{cp:x}\t{w}\t{h}\t{sha}\n' for cp, w, h, sha in rows)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix('.tmp')
    temporary.write_text(text)
    temporary.replace(output)
    return len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--app', type=Path)
    source.add_argument('--romfs', type=Path, help='build folder containing romfs.bin and runtime-index.bin')
    parser.add_argument('--version', default='v143', choices=('v143',))
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(f'{prepare(args.app, args.version, args.output, args.romfs)} live HUD prompt fingerprints prepared')
