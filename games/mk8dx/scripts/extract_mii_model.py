#!/usr/bin/env python3
"""Extract the console's Mii parts (system data MiiModel, 0100000000000802) from your own Switch firmware.

    python3 scripts/extract_mii_model.py --keys prod.keys --firmware FIRMWARE_NCA_FOLDER --output <work>/extracted/firmware/mii-model

Mario Kart 8 Deluxe mounts this archive as MiiResource: and reads one texture and one shape file of it to draw the Mii
racers' heads (runtime/mii_model.c). It is the player's own firmware content, like the system fonts the Zelda build
extracts with games/totk/scripts/extract_shared_fonts.py, whose NCA reading (FS header and all six IVFC levels
verified) this uses as it is: the repository's Zelda scripts are imported, not copied. Without it the runtime writes an
empty stand-in and the heads draw nothing.

Writes every file of the archive into the output folder and manifest.json (name, size, SHA-256 of each)."""
import argparse
import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path

ZELDA = Path(__file__).resolve().parents[2] / 'totk/scripts'
sys.path.insert(0, str(ZELDA))
from extract_exefs import load_keys, read_at, require  # noqa: E402
from extract_romfs import index_romfs  # noqa: E402
from extract_shared_fonts import nca_header, nca_romfs  # noqa: E402

MII_MODEL = '0100000000000802'
# The files the game reads (main's resource name tables): textures low/mid, linear/sRGB, and shapes mid/high.
EXPECTED = ('NXTextureLowLinear.dat', 'NXTextureLowSRGB.dat', 'NXTextureMidLinear.dat', 'NXTextureMidSRGB.dat',
            'ShapeHigh.dat', 'ShapeMid.dat')


def find_nca(folder, keys, title=MII_MODEL):
    require(folder.is_dir(), 'Firmware must be a directory containing NCA files')
    found = []
    for path in sorted(folder.iterdir()):
        if path.is_dir():
            path = path / '00'
        if path.name.startswith('._') or not path.is_file() or (path.name != '00' and path.suffix.lower() != '.nca'):
            continue
        if path.name.endswith('.cnmt.nca'):
            continue
        with path.open('rb') as f:
            hdr = nca_header(f, keys)
        require(hdr[0x200:0x204] == b'NCA3', 'Invalid firmware NCA header (check prod.keys)')
        if hdr[0x205] in (4, 5) and f'{struct.unpack_from("<Q", hdr, 0x210)[0]:016x}' == title:
            found.append(path)
    require(len(found) == 1, f'expected one data NCA of {title} in the firmware (content type data or public data), found {len(found)}')
    return found[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--firmware', type=Path, required=True, help='unpacked firmware NCA directory, from your own Switch')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    require(not args.output.exists(), 'Output already exists')
    keys = load_keys(args.keys)
    nca = find_nca(args.firmware, keys)
    files = []
    with tempfile.TemporaryDirectory() as tmp:
        image = Path(tmp) / 'mii-model.romfs'
        nca_romfs(nca, keys, image)
        entries = index_romfs(image)
        names = {e['path'] for e in entries}
        missing = [n for n in EXPECTED if n not in names]
        require(not missing, 'MiiModel lacks ' + ', '.join(missing))
        args.output.mkdir(parents=True)
        with image.open('rb') as f:
            for e in entries:
                require('/' not in e['path'] and e['path'] not in ('.', '..'), f'unexpected path {e["path"]}')
                data = read_at(f, e['offset'], e['size'])
                (args.output / e['path']).write_bytes(data)
                files.append(dict(name=e['path'], size=len(data), sha256=hashlib.sha256(data).hexdigest()))
    manifest = dict(status='pass', title_id=MII_MODEL, source=nca.name,
                    integrity='NCA FS header and six IVFC levels verified; RSA not checked', files=files)
    (args.output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(dict(status='pass', files=[(f['name'], f['size']) for f in files])))


if __name__ == '__main__':
    main()
