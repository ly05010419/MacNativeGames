"""Open-source stand-ins for the six nn::pl shared fonts, made once per Mac into the deps cache, for a game whose player has
no firmware fonts to give (Ocarina of Time: a Switch 2 dump holds none, and players who never built Tears of the Kingdom or
Breath of the Wild have no extracted ones; user 2026-11-16: "经常有人 安装 oot 缺字体 用这种方式 继续安装").

    from tools.deps.open_fonts import open_fonts
    open_fonts()    # a folder with 0-standard.bfttf … 5-nintendo-extension.bfttf, manifest.json and OFL.txt

The fonts are Noto (google/fonts at the commit pins.py names, SIL OFL 1.1), each checked against its SHA-256; the
variable ones are pinned at Regular (weight 400) by fontTools (its pinned wheel, in a virtual environment of its own) and
saved as static TrueType. Each file is a BFTTF with key 0, so its payload is the plain font both kinds of runtime hand the
game: games/oot/runtime/shared_font.c gives the file 8 bytes past its start, games/kirby/runtime/shared_font.c decodes by
the header's key first. build_app.py font_report binds their bytes like fonts of another firmware version.
Slot 5 (Nintendo's button icons) has no open equivalent: Noto Sans Symbols 2 stands in, the icons themselves are missing.

Run as a script by the fontTools environment's Python (-I): open_fonts.py <plan.json> <output folder>.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

DECODED_MAGIC = 0x7F9A0218          # a BFTTF header word XORed with the key; the key is 0 here
SFNT_TRUETYPE = 0x00010000
MAX_FILE_BYTES = 64 << 20           # games/*/runtime/shared_font.c refuses larger files
REGULAR_WEIGHT = 400
FORMAT = 1                          # changes the cache folder when what is made here changes


def bfttf(payload):
    """The BFTTF file of a font: an 8-byte big-endian header (magic, size) XORed with key 0, then the font, padded to
    32-bit words as the format counts them."""
    payload = payload + bytes(-len(payload) % 4)
    return struct.pack('>II', DECODED_MAGIC, len(payload)) + payload


def write_slots(slots, output, license_file=None):
    """output/<type>-<name>.bfttf for each (type, name, source, font bytes) and the manifest.json build_app.py
    validate_fonts compares with them (type, name, bfttf_bytes, font_bytes, sha256). Returns the manifest."""
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    rows = []
    for kind, name, source, payload in slots:
        data = bfttf(payload)
        if struct.unpack_from('>I', data, 8)[0] != SFNT_TRUETYPE:
            raise ValueError(f'{source}: not a TrueType font')
        if len(data) >= MAX_FILE_BYTES:
            raise ValueError(f'{source}: {len(data)} bytes as a BFTTF, over the runtime limit of {MAX_FILE_BYTES}')
        (output / f'{kind}-{name}.bfttf').write_bytes(data)
        rows.append(dict(type=kind, name=name, source=source, bfttf_bytes=len(data), font_bytes=len(data) - 8,
                         sha256=hashlib.sha256(data).hexdigest()))
    if [r['type'] for r in rows] != list(range(6)):
        raise ValueError('the six shared-font slots 0-5, in order')
    manifest = dict(status='pass', origin='open-source Noto fonts (SIL OFL 1.1), not extracted from a firmware',
                    integrity='each file is a BFTTF (key 0) whose payload is a static TrueType font', fonts=rows)
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    if license_file:
        (output / 'OFL.txt').write_bytes(Path(license_file).read_bytes())
    return manifest


def static_truetype(source):
    """A static Regular TrueType font: a variable font pinned at weight 400 (fontTools; only in its own environment)."""
    from io import BytesIO
    from fontTools.ttLib import TTFont
    from fontTools.varLib import instancer
    font = TTFont(source, recalcTimestamp=False)    # the same bytes on every Mac
    if 'glyf' not in font:
        raise ValueError(f'{source}: not a TrueType (glyf) font')
    if 'fvar' in font:
        font = instancer.instantiateVariableFont(font, {'wght': REGULAR_WEIGHT})
    out = BytesIO()
    font.save(out)
    return out.getvalue()


def convert(plan, output):
    """plan.json: {"slots": [[type, name, source path], …], "license": path}. Each source is converted once."""
    plan = json.loads(Path(plan).read_text())
    made = {}
    slots = []
    for kind, name, source in plan['slots']:
        if source not in made:
            print(f'converting {Path(source).name}', flush=True)
            made[source] = static_truetype(source)
        slots.append((kind, name, Path(source).name, made[source]))
    write_slots(slots, output, plan.get('license'))


def _url(path):
    from urllib.parse import quote
    from .pins import OPEN_FONTS_COMMIT
    return f'https://raw.githubusercontent.com/google/fonts/{OPEN_FONTS_COMMIT}/ofl/{quote(path)}'


def _fonttools_python():
    """The Python of a virtual environment holding only the pinned fontTools wheel (pure Python: any 3.9 or later, so
    it is made with the build's own Python, not only 3.14 as python_environment)."""
    import shutil
    from . import venv_python
    from .core import cache, fetch, locked, run
    from .pins import FONTTOOLS
    name, url, digest = FONTTOOLS
    venv = cache() / f'fonttools-{digest[:12]}'
    python, done = venv / 'bin/python', venv / '.done'
    if done.is_file() and python.is_file():
        return python
    with locked():
        if done.is_file() and python.is_file():
            return python
        wheel = fetch(url, digest, name)
        if venv.exists():
            shutil.rmtree(venv)
        run([venv_python(), '-m', 'venv', venv], 'fonttools-venv.log')
        run([python, '-m', 'pip', 'install', '--no-index', '--no-deps', '--disable-pip-version-check', wheel],
            'fonttools-pip.log')
        done.touch()
    return python


def open_fonts():
    """The cache folder of the six open-source shared fonts, made the first time (about 50 MB downloaded)."""
    from .core import cache, fetch, install, locked, run, say
    from .pins import FONTTOOLS, OPEN_FONTS, OPEN_FONTS_COMMIT, OPEN_FONTS_LICENSE
    identity = hashlib.sha256(repr((FORMAT, OPEN_FONTS_COMMIT, OPEN_FONTS, OPEN_FONTS_LICENSE, FONTTOOLS)).encode()).hexdigest()[:12]
    folder = cache() / f'open-fonts-{identity}'
    if (folder / 'manifest.json').is_file():
        return folder
    with locked():
        if (folder / 'manifest.json').is_file():
            return folder
        say('making the six shared fonts from open-source Noto fonts (SIL OFL 1.1), once for this Mac')
        fetched = {path: fetch(_url(path), digest, 'google-fonts-' + Path(path).name) for _, _, path, digest in OPEN_FONTS}
        license_file = fetch(_url(OPEN_FONTS_LICENSE[0]), OPEN_FONTS_LICENSE[1], 'google-fonts-OFL.txt')
        python = _fonttools_python()
        plan = cache() / 'build' / 'open-fonts-plan.json'
        plan.parent.mkdir(parents=True, exist_ok=True)
        plan.write_text(json.dumps(dict(slots=[[kind, name, str(fetched[path])] for kind, name, path, _ in OPEN_FONTS],
                                        license=str(license_file))))
        install(folder, lambda stage: run([python, '-I', '-B', Path(__file__).resolve(), plan, stage], 'open-fonts.log'))
    return folder


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: open_fonts.py <plan.json> <output folder>')
    convert(sys.argv[1], sys.argv[2])
