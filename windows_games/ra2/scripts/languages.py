"""Chinese for Red Alert 2 and Yuri's Revenge, from your own language pack.

    python3 -I scripts/languages.py <game dir> <Traditional Chinese pack dir>

The Steam release supports Traditional Chinese officially (menus and
subtitles): its language pack (depot 2229855, the same files as the English
install but for LANGUAGE.MIX and LANGMD.MIX) carries the string tables
RA2.CSF and RA2MD.CSF in Traditional Chinese. This writes, into
<game dir>/lang/:

- zh-Hant/RA2.CSF, RA2MD.CSF: those tables as they are (the game's own font
  has every Big5 glyph);
- zh-Hans/RA2.CSF, RA2MD.CSF: the same in Simplified Chinese with mainland
  wording (OpenCC's tw2sp: brew install opencc), and GAME.FNT, the game font
  with the GB2312 glyphs Big5 lacks drawn from a system font.

The game is then played in the language chosen (src/ra2/game_dir.h: the
system's, --language en|zh-Hant|zh-Hans, or on iOS the button when choosing
the game), English being the install's own. The game font (GAME.FNT in
RA2.MIX's LOCAL.MIX, the same in every language: 1 bit a pixel, 16 rows of
24, a width byte first; glyph n is the (n-1)th record, 0 is none) maps every
CJK ideograph, but those outside Big5 (most simplified ones) to one
placeholder glyph; those of GB2312 are drawn from Source Han Sans / Noto Sans
CJK SC DemiLight (SIL Open Font License 1.1, so the glyphs may be embedded;
downloaded at a pinned commit and checked, kept in
~/Library/Caches/WinNativeGames/fonts or RA2_FONT_CACHE) at 15 px, like the
font's own. The MIX public key is read from your own game.exe. Everything
written is made from your files: never commit or share it.

Get the pack with steamcmd, logged in to your own account:
    steamcmd +@sSteamCmdForcePlatformType windows +login <you> +download_depot 2229850 2229855 +quit
"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True  # nothing written into the source folder
sys.path.insert(0, str(Path(__file__).resolve().parent))
import csf  # noqa: E402
import mixls  # noqa: E402

# Newlines inside strings go through OpenCC as this private-use character,
# so that one string is one line.
NEWLINE = '\ue000'

# Westwood's public key for encrypted MIX headers, read from the player's own
# game.exe in <game dir> (mixls.public_key); set in main.
PUBLIC_KEY = None

# Noto Sans CJK SC (Source Han Sans), DemiLight: the weight closest to the
# game font's own strokes at 15 px. SIL Open Font License 1.1 (LICENSE at
# the same commit).
FONT_COMMIT = '165c01b46ea533872e002e0785ff17e44f6d97d8'
FONT_NAME = 'NotoSansSC-DemiLight.otf'
FONT_URL = (f'https://raw.githubusercontent.com/notofonts/noto-cjk/{FONT_COMMIT}/'
            f'Sans/SubsetOTF/SC/{FONT_NAME}')
FONT_SHA256 = '63a895a808a155fe69d67f742e60ee708fe0eaaef0650904bb4eead3941e98dc'


def font_path():
    """The pinned font, downloaded once and checked against its SHA-256."""
    import hashlib
    import os
    import urllib.request
    cache = Path(os.environ.get('RA2_FONT_CACHE',
                                Path.home() / 'Library/Caches/WinNativeGames/fonts'))
    path = cache / f'{FONT_COMMIT[:12]}-{FONT_NAME}'
    if not path.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        print(f'downloading {FONT_NAME} (Noto Sans CJK, SIL OFL 1.1)')
        try:
            data = urllib.request.urlopen(FONT_URL, timeout=120).read()
        except OSError as e:
            sys.exit(f'cannot download {FONT_URL}: {e}')
        if hashlib.sha256(data).hexdigest() != FONT_SHA256:
            sys.exit(f'{FONT_URL}: not the expected file (SHA-256 differs)')
        tmp = path.with_suffix('.part')
        tmp.write_bytes(data)
        tmp.replace(path)
    elif hashlib.sha256(path.read_bytes()).hexdigest() != FONT_SHA256:
        sys.exit(f'{path}: not the expected file (SHA-256 differs); delete it to download again')
    return path
GLYPH_PX = 15  # the width and height of the font's own CJK glyphs


def member(data, name, where):
    entry = mixls.parse(data, PUBLIC_KEY).get(mixls.mix_id(name))
    if not entry:
        sys.exit(f'{where} has no {name}')
    off, size = entry
    return data[off:off + size]


def table_from_mix(path, name='RA2.CSF'):
    return csf.parse(member(Path(path).read_bytes(), name, path))


def in_dir(game_dir, name):
    """The file `name` in game_dir, in whatever case it is spelled there."""
    for f in Path(game_dir).iterdir():
        if f.name.lower() == name.lower():
            return f
    sys.exit(f'{game_dir} has no {name}')


def draw_glyph(font, ch):
    """`ch` as the font's glyph record: width, then 16 rows of 3 bytes."""
    from PIL import Image, ImageDraw
    im = Image.new('L', (24, 16), 0)
    left, top, right, bottom = font.getbbox(ch)
    x = (GLYPH_PX - (right - left)) // 2 - left
    y = 1 + (GLYPH_PX - (bottom - top)) // 2 - top
    ImageDraw.Draw(im).text((x, y), ch, fill=255, font=font)
    rows = bytearray()
    for py in range(16):
        bits = 0
        for px in range(24):
            bits = bits << 1 | (im.getpixel((px, py)) >= 110)
        rows += bits.to_bytes(3, 'big')
    return bytes([GLYPH_PX]) + bytes(rows)


def font_with_simplified(fnt):
    from PIL import ImageFont
    magic, field, row_bytes, rows, line, count, glyph_size = struct.unpack_from('<4s6I', fnt, 0)
    if magic != b'fonT' or (row_bytes, rows, glyph_size) != (3, 16, 49):
        sys.exit(f'GAME.FNT: unexpected format {magic} {row_bytes}x{rows}, {glyph_size} a glyph')
    cmap = list(struct.unpack_from('<65536H', fnt, 28))
    placeholder = cmap[0x9879]  # 项, outside Big5
    gb2312 = [bytes([hi, lo]).decode('gb2312', 'ignore') for hi in range(0xB0, 0xF8)
              for lo in range(0xA1, 0xFF)]
    missing = sorted({ch for ch in gb2312 if ch and cmap[ord(ch)] in (0, placeholder)})
    font = ImageFont.truetype(str(font_path()), GLYPH_PX)
    glyphs = bytearray(fnt[28 + 131072:])
    for i, ch in enumerate(missing):
        cmap[ord(ch)] = count + 1 + i
        glyphs += draw_glyph(font, ch)
    header = struct.pack('<4s6I', magic, field, row_bytes, rows, line, count + len(missing),
                         glyph_size)
    return header + struct.pack('<65536H', *cmap) + bytes(glyphs), len(missing)


def write_into(game_dir, name, data):
    out = Path(game_dir) / name
    with tempfile.NamedTemporaryFile(dir=out.parent, delete=False) as t:
        t.write(data)
    Path(t.name).replace(out)
    return out


def to_simplified(texts):
    joined = '\n'.join(t.replace('\n', NEWLINE) for t in texts)
    try:
        r = subprocess.run(['opencc', '-c', 'tw2sp.json'], input=joined, capture_output=True,
                           text=True, check=True)
    except FileNotFoundError:
        sys.exit('opencc not found: brew install opencc')
    out = r.stdout.split('\n')
    if out and out[-1] == '' and len(out) == len(texts) + 1:
        out.pop()
    if len(out) != len(texts):
        sys.exit(f'opencc returned {len(out)} strings for {len(texts)}')
    return [t.replace(NEWLINE, '\n') for t in out]


def simplified(table):
    """The table with every string in Simplified Chinese."""
    texts = [text for _, values in table['labels'] for text, _ in values]
    out = iter(to_simplified(texts))
    return dict(table, labels=[(name, [(next(out), extra) for _, extra in values])
                               for name, values in table['labels']]), len(texts)


def main():
    global PUBLIC_KEY
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().split('\n\n')[1])
    game_dir, pack = Path(sys.argv[1]), Path(sys.argv[2])
    PUBLIC_KEY = mixls.public_key(game_dir)
    tables = [('RA2.CSF', in_dir(pack, 'LANGUAGE.MIX'))]
    if any(f.name.lower() == 'langmd.mix' for f in game_dir.iterdir()):
        tables.append(('RA2MD.CSF', in_dir(pack, 'LANGMD.MIX')))
    hant, hans = game_dir / 'lang/zh-Hant', game_dir / 'lang/zh-Hans'
    hant.mkdir(parents=True, exist_ok=True)
    hans.mkdir(parents=True, exist_ok=True)
    for name, mix in tables:
        data = member(mix.read_bytes(), name, mix)
        table = csf.parse(data)
        if table['language'] != csf.LANGUAGE_CHINESE:
            sys.exit(f'{mix}: its {name} is language {table["language"]}, not Chinese (9)')
        out = write_into(hant, name, data)
        print(f'{out}: {len(table["labels"])} labels, Traditional Chinese')
        zh, count = simplified(table)
        out = write_into(hans, name, csf.serialize(zh))
        print(f'{out}: {count} strings in Simplified Chinese')
    ra2_mix = in_dir(game_dir, 'RA2.MIX')
    local = member(ra2_mix.read_bytes(), 'LOCAL.MIX', ra2_mix)
    fnt, added = font_with_simplified(member(local, 'GAME.FNT', f'{ra2_mix}/LOCAL.MIX'))
    out = write_into(hans, 'GAME.FNT', fnt)
    print(f'{out}: {added} simplified glyphs added')


if __name__ == '__main__':
    main()
