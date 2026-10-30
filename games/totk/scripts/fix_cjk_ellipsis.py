#!/usr/bin/env python3
"""The CJK ellipsis in Tears of the Kingdom sits above the text on the Mac; this moves it back to the middle.

The game's own CJK fonts live in the RomFS (`Font/Font_CNzh.Nin_NX_NVN.bfarc.zs` and its TWzh / KRko / default
siblings), as BFTTF/ BFOTF members of a SARC. Their U+2026 (…) and U+2025 (‥) glyphs are drawn high in the em
box (yMin around 300 of 1024 units), and on the Mac that renders above the line instead of at its middle, unlike a
retail console. This script shifts those two glyphs down by `--delta` units in every CJK font of every archive and writes
the archives into a mod folder; nothing of the game is in this repository - the script reads the player's own RomFS.

The archives are written at exactly their original size (the font member is padded after re-saving), so the RESTBL needs
no merge: install the folder with make_mod_manifest.py as usual.

    python3.14 games/totk/scripts/fix_cjk_ellipsis.py --work ../TOTK_build --version v121 \
        --output ~/Library/Application\\ Support/MacNativeGames/TOTK/mods/v121

Needs Python 3.14 (compression.zstd) and fontTools (pip install fonttools).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

try:
    import compression.zstd as zstd
except ImportError:
    sys.exit('needs Python 3.14 (compression.zstd)')
try:
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit('needs fontTools: pip install fonttools')

ARCHIVES = ('Font/Font.Nin_NX_NVN.bfarc.zs', 'Font/Font_CNzh.Nin_NX_NVN.bfarc.zs',
            'Font/Font_TWzh.Nin_NX_NVN.bfarc.zs', 'Font/Font_KRko.Nin_NX_NVN.bfarc.zs')
MAGIC = 0x7f9a0218
GLYPHS = (0x2026, 0x2025)          # … and ‥
HIGH = 200                         # only glyphs drawn in the upper part of the em box


def sarc_members(data):
    """name -> (absolute start, end) of every SARC member."""
    header, size, start = struct.unpack_from('<HxxII', data, 4)
    count = struct.unpack_from('<H', data, header + 6)[0]
    names = header + 12 + count * 16 + 8
    out = {}
    for i in range(count):
        _, attribute, lo, hi = struct.unpack_from('<4I', data, header + 12 + i * 16)
        off = names + (attribute & 0xffffff) * 4
        out[data[off:data.index(0, off, start)].decode()] = (start + lo, start + hi)
    return out


def decode_font(blob):
    """BFTTF/BFOTF: big-endian words XORed with a key derived from the first word; returns (sfnt, key)."""
    key = struct.unpack_from('>I', blob, 0)[0] ^ MAGIC
    size = int.from_bytes(struct.pack('<I', struct.unpack_from('>I', blob, 4)[0] ^ key), 'big')
    size = min(size, len(blob) - 8)
    body = blob[8:8 + size]
    out = bytearray()
    for i in range(0, size - size % 4, 4):
        out += struct.pack('>I', struct.unpack_from('>I', body, i)[0] ^ key)
    out += body[size - size % 4:]
    return bytes(out), key


def encode_font(sfnt, key):
    size = len(sfnt)
    out = bytearray(struct.pack('>I', MAGIC ^ key) + struct.pack('>I', struct.unpack('>I', size.to_bytes(4, 'big'))[0] ^ key))
    for i in range(0, size - size % 4, 4):
        out += struct.pack('>I', struct.unpack_from('>I', sfnt, i)[0] ^ key)
    out += sfnt[size - size % 4:]
    return bytes(out)


def shift(font, delta):
    """Move U+2026/U+2025 down by delta units; returns the moved (codepoint, yMin, yMax) tuples."""
    cmap = font.getBestCmap() if hasattr(font, 'getBestCmap') else font
    moved = []
    if 'glyf' not in font:
        return moved
    for cp in GLYPHS:
        name = cmap.get(cp)
        if not name:
            continue
        g = font['glyf'][name]
        if g.numberOfContours <= 0 or g.yMin <= HIGH:
            continue
        for i in range(len(g.coordinates)):
            x, y = g.coordinates[i]
            g.coordinates[i] = (x, y + delta)
        g.yMin += delta
        g.yMax += delta
        moved.append((hex(cp), g.yMin, g.yMax))
    return moved


def fix_archive(arc, delta):
    """The SARC with every CJK font's ellipsis moved; each member keeps its size (no RESTBL change)."""
    arc = bytearray(arc)
    report = []
    for name, (lo, hi) in sarc_members(bytes(arc)).items():
        if not name.endswith(('.bfttf', '.bfotf')):
            continue
        blob = bytes(arc[lo:hi])
        sfnt, key = decode_font(blob)
        tmp = Path('/tmp') / 'cjk-ellipsis.ttf'
        tmp.write_bytes(sfnt)
        try:
            font = TTFont(str(tmp))
        except Exception:
            continue
        moved = shift(font, delta)
        if not moved:
            continue
        try:
            font.save(str(tmp))
        except Exception as error:                    # a font with a glyph fontTools cannot recompile
            report.append((name, moved, f'left alone: {error}'))
            continue
        data = tmp.read_bytes()
        target = len(blob) - 8                        # the member's sfnt bytes, without the 8-byte header
        if len(data) > target:
            report.append((name, moved, 're-saved larger, left alone'))
            continue
        data += b'\0' * (target - len(data))         # same size: the RESTBL entry stays valid
        new = encode_font(data, key)
        assert len(new) == len(blob)
        arc[lo:hi] = new
        report.append((name, moved, 'moved'))
    return bytes(arc), report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument('--work', type=Path, help='the build work folder (its extracted/<version>/romfs)')
    src.add_argument('--romfs', type=Path, help='a folder holding romfs.bin and index.json')
    ap.add_argument('--version', default='v121', help='the game version inside the work folder (default v121)')
    ap.add_argument('--output', type=Path, required=True, help='the mod folder to write Font/*.zs into')
    ap.add_argument('--delta', type=int, default=-300, help='units to move the glyph down (default -300 of 1024)')
    args = ap.parse_args()

    romfs = args.romfs if args.romfs else args.work / 'extracted' / args.version / 'romfs'
    index = json.loads((romfs / 'index.json').read_text())
    files = {f['path']: f for f in index['files']}
    image = romfs / 'romfs.bin'

    def read(path):
        f = files[path]
        with image.open('rb') as fh:
            fh.seek(f['offset'])
            return fh.read(f['size'])

    pack = zstd.decompress(read('Pack/ZsDic.pack.zs'))
    dicts = {n: zstd.ZstdDict(pack[lo:hi]) for n, (lo, hi) in sarc_members(pack).items()}
    written = 0
    for path in ARCHIVES:
        if path not in files:
            print(f'{path}: not in this version, skipped')
            continue
        raw = read(path)
        arc = zstd.decompress(raw, zstd_dict=dicts['zs.zsdic'])
        fixed, report = fix_archive(arc, args.delta)
        if len(fixed) != len(arc):
            sys.exit(f'{path}: the archive changed size; refusing')
        out = args.output / path
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(zstd.compress(fixed, 3, zstd_dict=dicts['zs.zsdic']))
        written += 1
        for name, moved, what in report:
            print(f'{path}: {name.split("/")[-1]}: {moved} {what}')
    print(f'{written} archives written to {args.output}')
    if written:
        print('Install with make_mod_manifest.py (the archives keep their original size, so no RESTBL merge is needed).')


if __name__ == '__main__':
    main()
