#!/usr/bin/env python3
"""Build the sharp UI texture pack of Breath of the Wild 1.9.0 from a harvest folder, with AI upscalers.

The app's texture pack (games/botw/gfx/botw_texpack.h, docs/mods.md) draws the UI's small textures from larger PNGs. Run the game once with
BOTW_TEXPACK_HARVEST=<harvest folder>: it writes every UI texture it draws there as <key>.png, with a line in harvest.tsv. This tool turns
those pictures into pack pictures, <pack folder>/<key>.png, 2x or 4x larger, with the realesrgan-ncnn-vulkan program you installed:
  masks (R8 and BC4 grey, RG8 and BC5 two-channel)   the model realesrgan-x4plus-anime, one channel at a time (each channel is shown to the
                                                       model as a grey picture, the three results are averaged back to one channel)
  colour (RGBA8, BC1 to BC3, BC7)                    the model 4x-UltraSharp for the colour, and the mask model for the alpha
The factor is 4 for a picture of at most 128 texels on its longest side, else 2. The model always runs at 4x; a 2x picture is the result
reduced by a 2x2 box average. Each mask channel (and the alpha) is then capped at the source's 3x3 local maximum, so a grey line stays grey
(the model draws it white). Skipped unless overrides.tsv gives a model or scale: pictures of at most 16 texels on their longest side (9-slice
frame pieces and dots) and soft pictures (glows and shadows, sharpness below 0.6), which the model would make hard. A pack picture has the layout of its harvest picture (grey; RGB with blue 0 for two channels; RGBA, not premultiplied,
sRGB colour as stored). <pack folder>/ai-manifest.tsv lists what this tool wrote (key, models, factor, source mtime, processing revision): a picture there is
rebuilt only when its entry is out of date (or with --force), and a picture that is not listed (written by botw_prompts-style vector tools) is never overwritten.
overrides.tsv (in the harvest folder, or --overrides): lines "key<TAB>value", # starts a comment. The value is skip, a model name, 2x or 4x, or
"model,2x". A model name replaces the mask model for masks and the colour model for colour (the alpha keeps the mask model).
Nothing is downloaded: the program and its models (<models folder>/<model>.param and .bin) come from you; see docs/mods.md.

Needs Python 3.14 and no other package.
usage: botw_texpack.py build --harvest <folder> --out <pack folder> --upscaler <realesrgan-ncnn-vulkan> --models <models folder>
                             [--overrides <tsv>] [--force] [--only <key,...>] [--dry-run]
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import NamedTuple

M64 = (1 << 64) - 1
MASK_MODEL = 'realesrgan-x4plus-anime'
COLOUR_MODEL = '4x-UltraSharp'
SMALL = 128                  # a picture up to this size on its longest side is made 4x, a larger one 2x
TINY = 16                    # a picture up to this size on its longest side is skipped: 9-slice frame pieces and dots, whose lines the model widens
SOFT = 0.6                   # a picture whose sharpness (see sharpness()) is below this is skipped: soft glows and shadows, which the model makes hard
PAD = 4                      # texels of edge copy around the picture while the model runs
MANIFEST = 'ai-manifest.tsv'
RG_METAL = (30, 142)         # RG8Unorm, BC5: the two-channel masks (the other masks are one channel)
REVISION = '2'               # the processing after the model (2: cap_tones); a manifest row of another revision is rebuilt
TOOL = {'upscaler': None, 'models': None}        # set by main(); run_upscaler reads it


class TexpackError(Exception):
    pass


# --- the runtime's key (pkg_hash in games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m) ---

def pkg_hash(data: bytes) -> int:
    """The runtime's content hash of a texture's guest bytes: four 64-bit lanes over 32-byte chunks, an FNV-style tail, a final mix."""
    n = len(data)
    a = [(0x9E3779B97F4A7C15 ^ n) & M64, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5]
    body = n - n % 32
    for words in struct.iter_unpack('<4Q', data[:body]):
        for k in range(4):
            x = ((a[k] ^ words[k]) * 0xff51afd7ed558ccd) & M64
            a[k] = x ^ (x >> 29)
    h = a[0] ^ ((a[1] * 3) & M64) ^ ((a[2] * 5) & M64) ^ ((a[3] * 7) & M64)
    for byte in data[body:]:
        h = ((h ^ byte) * 0x100000001b3) & M64
    h ^= h >> 33
    h = (h * 0xc4ceb9fe1a85ec53) & M64
    return h ^ (h >> 33)


def texpack_key(data: bytes, width: int, height: int, nvn_format: int) -> str:
    """The pack key (and file name without .png) of a texture: "<hash 16 hex>-<w>x<h>-<format 2 hex>"."""
    return '%016x-%dx%d-%02x' % (pkg_hash(data), width, height, nvn_format)


# --- PNG: 8-bit grey, grey + alpha, RGB, RGBA; not interlaced ---

class Image(NamedTuple):
    width: int
    height: int
    channels: int            # 1 grey, 2 grey + alpha, 3 RGB, 4 RGBA
    data: bytes              # tight rows, channels interleaved


COLOUR_TYPE = {1: 0, 2: 4, 3: 2, 4: 6}
CHANNELS = {value: key for key, value in COLOUR_TYPE.items()}


def read_png(path) -> Image:
    blob = Path(path).read_bytes()
    if blob[:8] != b'\x89PNG\r\n\x1a\n':
        raise TexpackError(f'{path}: not a PNG')
    pos, header, idat = 8, None, []
    while pos + 8 <= len(blob):
        size, tag = struct.unpack_from('>I4s', blob, pos)
        body = blob[pos + 8:pos + 8 + size]
        if len(body) != size or pos + 12 + size > len(blob) or zlib.crc32(tag + body) != struct.unpack_from('>I', blob, pos + 8 + size)[0]:
            raise TexpackError(f'{path}: damaged PNG chunk {tag!r}')
        pos += 12 + size
        if tag == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif tag == b'IDAT':
            idat.append(body)
        elif tag == b'IEND':
            break
    if header is None or not idat:
        raise TexpackError(f'{path}: no image data')
    width, height, depth, ctype, _, _, interlace = header
    if depth != 8 or ctype not in CHANNELS or interlace:
        raise TexpackError(f'{path}: only 8-bit grey, grey + alpha, RGB and RGBA PNGs without interlacing are read (bit depth {depth}, colour type {ctype}, interlace {interlace})')
    nc = CHANNELS[ctype]
    stride = width * nc
    try:
        raw = zlib.decompress(b''.join(idat))
    except zlib.error as error:
        raise TexpackError(f'{path}: {error}') from None
    if len(raw) < height * (stride + 1):
        raise TexpackError(f'{path}: image data is short')
    out, prev, pos = bytearray(), bytes(stride), 0
    for _ in range(height):
        kind, line = raw[pos], bytearray(raw[pos + 1:pos + 1 + stride])
        pos += stride + 1
        if kind == 1:
            for i in range(nc, stride):
                line[i] = (line[i] + line[i - nc]) & 255
        elif kind == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif kind == 3:
            for i in range(stride):
                line[i] = (line[i] + (((line[i - nc] if i >= nc else 0) + prev[i]) >> 1)) & 255
        elif kind == 4:
            for i in range(stride):
                a, b, c = (line[i - nc] if i >= nc else 0), prev[i], (prev[i - nc] if i >= nc else 0)
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind != 0:
            raise TexpackError(f'{path}: unknown PNG filter {kind}')
        out += line
        prev = line
    return Image(width, height, nc, bytes(out))


def write_png(path, image: Image):
    width, height, nc, data = image
    if len(data) != width * height * nc:
        raise TexpackError(f'{path}: {len(data)} bytes for a {width}x{height} picture of {nc} channels')
    stride = width * nc
    raw = b''.join(b'\0' + data[y * stride:(y + 1) * stride] for y in range(height))
    def chunk(tag, body):
        return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, COLOUR_TYPE[nc], 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


# --- planes: one channel as w*h bytes ---

def interleave(planes) -> bytes:
    out = bytearray(len(planes[0]) * len(planes))
    for c, plane in enumerate(planes):
        out[c::len(planes)] = plane
    return bytes(out)


def pad_plane(plane, w, h, pad):
    """The plane with `pad` copies of its edge texels on every side."""
    rows = [plane[y * w:(y + 1) * w] for y in range(h)]
    rows = [r[:1] * pad + r + r[-1:] * pad for r in rows]
    return b''.join([rows[0]] * pad + rows + [rows[-1]] * pad)


def crop_plane(plane, w, h, border):
    return b''.join(plane[y * w + border:(y + 1) * w - border] for y in range(border, h - border))


def box_reduce(plane, w, h, k=2):
    """Each k x k block of the plane replaced by its rounded mean."""
    if w % k or h % k:
        raise TexpackError(f'{w}x{h} does not divide by {k}')
    rows = [plane[y * w:(y + 1) * w] for y in range(h)]
    out, area = bytearray(), k * k
    for y in range(0, h, k):
        cols = [rows[y + j][i::k] for j in range(k) for i in range(k)]
        out += bytes((sum(t) + area // 2) // area for t in zip(*cols))
    return bytes(out)


def mean_plane(planes):
    n = len(planes)
    return bytes((sum(t) + n // 2) // n for t in zip(*planes))


def local_max(plane, w, h):
    """Each texel replaced by the largest value of its 3x3 neighbourhood."""
    rows = [plane[y * w:(y + 1) * w] for y in range(h)]
    across = [bytes(max(r[max(x - 1, 0):x + 2]) for x in range(w)) for r in rows]
    return b''.join(bytes(map(max, across[max(y - 1, 0)], across[y], across[min(y + 1, h - 1)])) for y in range(h))


def cap_tones(big, plane, w, h, k):
    """The k-times larger plane `big` limited, texel by texel, to the source's 3x3 local maximum. The model draws a mid-grey line or patch as
    white; the cap keeps it at the source's grey and leaves white shapes, edges and smooth gradients as the model made them."""
    cap = local_max(plane, w, h)
    out = bytearray()
    for y in range(h * k):
        row = cap[(y // k) * w:(y // k + 1) * w]
        out += bytes(map(min, big[y * w * k:(y + 1) * w * k], bytes(v for v in row for _ in range(k))))
    return bytes(out)


def sharpness(plane, w, h):
    """The largest step between neighbouring texels over the plane's value range: below SOFT the picture is a soft glow or shadow, None if flat."""
    lo, hi = min(plane), max(plane)
    if lo == hi:
        return None
    step = max((abs(a - b) for y in range(h) for a, b in zip(plane[y * w:(y + 1) * w], plane[y * w + 1:(y + 1) * w])), default=0)
    step = max(step, max((abs(a - b) for a, b in zip(plane[:-w], plane[w:])), default=0))
    return step / (hi - lo)


def run_upscaler(model, in_path, out_path):
    """realesrgan-ncnn-vulkan at 4x: the only place that starts the program (tests replace it). Needs TOOL set; <models>/<model>.param and .bin must exist."""
    upscaler, models = TOOL['upscaler'], TOOL['models']
    for suffix in ('.param', '.bin'):
        if not (Path(models) / (model + suffix)).is_file():
            raise TexpackError(f'model {model}: {Path(models) / (model + suffix)} not found (nothing is downloaded; see docs/mods.md)')
    done = subprocess.run([str(upscaler), '-i', str(in_path), '-o', str(out_path), '-n', model, '-s', '4', '-m', str(models)], capture_output=True, text=True)
    if done.returncode or not Path(out_path).is_file():
        raise TexpackError(f'{Path(upscaler).name} failed on model {model} (exit {done.returncode}): {done.stderr.strip()[-300:]}')


def upscale_rgb(model, planes, w, h, tmp, name):
    """Three planes of w x h through the model at 4x -> three planes of 4w x 4h. A flat picture needs no model.
    The model runs on a copy padded with PAD copies of the edge texels, cropped back after: at a bare picture edge the model sees an abrupt
    end of the image and darkens or rings the outermost texels, which would show as a dim rim on a UI icon drawn over the world."""
    if all(len(set(p)) == 1 for p in planes):
        return [bytes([p[0]]) * (16 * w * h) for p in planes]
    pw, ph = w + 2 * PAD, h + 2 * PAD
    src, dst = Path(tmp) / f'{name}-in.png', Path(tmp) / f'{name}-out.png'
    write_png(src, Image(pw, ph, 3, interleave([pad_plane(p, w, h, PAD) for p in planes])))
    run_upscaler(model, src, dst)
    big = read_png(dst)
    if (big.width, big.height) != (4 * pw, 4 * ph) or big.channels < 3:
        raise TexpackError(f'the upscaler returned {big.width}x{big.height} with {big.channels} channels for {pw}x{ph}')
    return [crop_plane(big.data[c::big.channels], big.width, big.height, 4 * PAD) for c in range(3)]


def upscale_mask(model, plane, w, h, factor, tmp, name):
    """One mask channel: a grey picture shown to the model as RGB, the results averaged back to one channel."""
    grey = mean_plane(upscale_rgb(model, [plane] * 3, w, h, tmp, name))
    grey = box_reduce(grey, 4 * w, 4 * h) if factor == 2 else grey
    return cap_tones(grey, plane, w, h, factor)


def upscale_image(image: Image, layout, models, factor, tmp) -> Image:
    """layout: grey, rg (RGB with blue 0) or rgba; models: (mask model,) for masks, (colour model, mask model) for colour."""
    w, h, nc, data = image
    if layout == 'rgba':
        colour = upscale_rgb(models[0], [data[c::4] for c in range(3)], w, h, tmp, 'rgb')
        if factor == 2:
            colour = [box_reduce(p, 4 * w, 4 * h) for p in colour]
        alpha = upscale_mask(models[1], data[3::4], w, h, factor, tmp, 'alpha')
        return Image(w * factor, h * factor, 4, interleave([*colour, alpha]))
    chans = 1 if layout == 'grey' else 2
    planes = [upscale_mask(models[0], data[c::nc], w, h, factor, tmp, f'c{c}') for c in range(chans)]
    if layout == 'rg':
        planes.append(bytes(len(planes[0])))
    return Image(w * factor, h * factor, len(planes), interleave(planes))


# --- the harvest folder, overrides, plan ---

class Row(NamedTuple):
    key: str
    width: int
    height: int
    metal: int
    cls: str


class Override(NamedTuple):
    skip: bool = False
    model: str | None = None
    factor: int | None = None


def read_harvest(path):
    rows, seen = [], set()
    lines = Path(path).read_text().splitlines()
    head = lines[0].split('\t') if lines else []
    need = ('key', 'width', 'height', 'metal_format', 'class')
    if any(n not in head for n in need):
        raise TexpackError(f'{path}: the header must have the columns {", ".join(need)}')
    for number, line in enumerate(lines[1:], 2):
        if not line.strip():
            continue
        cells = dict(zip(head, line.split('\t')))
        try:
            row = Row(cells['key'], int(cells['width']), int(cells['height']), int(cells['metal_format']), cells['class'])
        except (KeyError, ValueError):
            raise TexpackError(f'{path}:{number}: cannot read this line') from None
        if row.cls not in ('mask', 'colour'):
            raise TexpackError(f'{path}:{number}: class must be mask or colour, not {row.cls}')
        if row.key not in seen:
            seen.add(row.key)
            rows.append(row)
    return rows


def parse_override(value):
    """skip | <model> | 2x | 4x | <model>,2x (in any order)."""
    skip, model, factor = False, None, None
    for token in (t.strip() for t in value.split(',')):
        if token == 'skip':
            skip = True
        elif token in ('2x', '4x'):
            if factor is not None:
                raise ValueError('two scales')
            factor = int(token[0])
        elif token and model is None:
            model = token
        else:
            raise ValueError('more than one model' if token else 'empty item')
    return Override(skip, model, factor)


def read_overrides(path):
    found = {}
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, tab, value = line.partition('\t')
        try:
            if not tab or not key.strip():
                raise ValueError('expected key<TAB>value')
            found[key.strip()] = parse_override(value)
        except ValueError as error:
            raise TexpackError(f'{path}:{number}: {error}') from None
    return found


def layout_of(row):
    return 'rgba' if row.cls == 'colour' else 'rg' if row.metal in RG_METAL else 'grey'


def choose(row, override=None):
    """(layout, models, factor) for a harvest row; an override wins over the class and size rules."""
    override = override or Override()
    layout = layout_of(row)
    models = (override.model or COLOUR_MODEL, MASK_MODEL) if layout == 'rgba' else (override.model or MASK_MODEL,)
    return layout, models, override.factor or (4 if max(row.width, row.height) <= SMALL else 2)


def read_manifest(path):
    found = {}
    if Path(path).is_file():
        for line in Path(path).read_text().splitlines()[1:]:
            cells = line.split('\t')
            if len(cells) in (4, 5):      # 4: before REVISION; such a row still marks the picture as this tool's, and is out of date
                found[cells[0]] = tuple(cells[1:])
    return found


def write_manifest(path, entries):
    text = 'key\tmodels\tfactor\tsource_mtime_ns\trevision\n' + ''.join('\t'.join((key, *entries[key])) + '\n' for key in sorted(entries))
    tmp = Path(str(path) + '.tmp')
    tmp.write_text(text)
    os.replace(tmp, path)


def build(harvest, out, overrides=None, force=False, only=None, dry_run=False, log=print):
    """Returns the number of pictures that failed. Existing pack files that the manifest does not list are never touched."""
    harvest, out = Path(harvest), Path(out)
    overrides_path = Path(overrides) if overrides else harvest / 'overrides.tsv'
    if overrides and not overrides_path.is_file():
        raise TexpackError(f'{overrides_path} not found')
    table = read_overrides(overrides_path) if overrides_path.is_file() else {}
    rows = read_harvest(harvest / 'harvest.tsv')
    known = {r.key for r in rows}
    if only is not None:
        wanted = set(only)
        unknown = wanted - known
        if unknown:
            raise TexpackError('not in harvest.tsv: ' + ', '.join(sorted(unknown)))
        rows = [r for r in rows if r.key in wanted]
    for key in sorted(set(table) - known):
        log(f'note: overrides.tsv names {key}, which is not in harvest.tsv')
    entries = read_manifest(out / MANIFEST)
    if not dry_run:
        out.mkdir(parents=True, exist_ok=True)
    counts = {'built': 0, 'current': 0, 'skipped': 0, 'failed': 0}
    for row in sorted(rows, key=lambda r: r.key):
        source, target = harvest / f'{row.key}.png', out / f'{row.key}.png'
        override = table.get(row.key)
        if override and override.skip:
            counts['skipped'] += 1; log(f'{row.key}: skip (overrides)'); continue
        if not source.is_file():
            counts['failed'] += 1; log(f'{row.key}: {source} not found'); continue
        if target.exists() and row.key not in entries:
            counts['skipped'] += 1; log(f'{row.key}: skip ({target.name} is not in {MANIFEST}; another tool wrote it)'); continue
        layout, models, factor = choose(row, override)
        try:
            image = read_png(source)
            want = {'grey': 1, 'rg': 3, 'rgba': 4}[layout]
            if (image.width, image.height, image.channels) != (row.width, row.height, want):
                raise TexpackError(f'{source.name} is {image.width}x{image.height} with {image.channels} channels; harvest.tsv says {row.width}x{row.height} ({layout}, {want} channels)')
        except TexpackError as error:
            counts['failed'] += 1; log(f'{row.key}: FAILED: {error}'); continue
        if layout == 'rg':       # RGB with blue 0: the two channels are red and green
            image = Image(image.width, image.height, 2, interleave([image.data[0::3], image.data[1::3]]))
        if not (override and (override.model or override.factor)):      # a model or a scale in overrides.tsv builds the picture anyway
            sharp = [s for c in range(image.channels) if (s := sharpness(image.data[c::image.channels], image.width, image.height)) is not None]
            reason = ('small' if max(row.width, row.height) <= TINY else 'flat' if not sharp
                      else f'soft, sharpness {max(sharp):.2f}' if max(sharp) < SOFT else None)
            if reason:
                counts['skipped'] += 1; log(f'{row.key}: skip ({reason}; a model or scale in overrides.tsv builds it)'); continue
        entry = ('+'.join(models), str(factor), str(source.stat().st_mtime_ns), REVISION)
        if not force and entries.get(row.key) == entry and target.is_file():
            counts['current'] += 1; log(f'{row.key}: current'); continue
        if dry_run:
            counts['built'] += 1; log(f'{row.key}: would build {entry[0]} x{factor}'); continue
        try:
            with tempfile.TemporaryDirectory() as tmp:
                result = upscale_image(image, layout, models, factor, tmp)
            write_png(str(target) + '.tmp', result)
            os.replace(str(target) + '.tmp', target)
        except TexpackError as error:
            counts['failed'] += 1; log(f'{row.key}: FAILED: {error}'); continue
        entries[row.key] = entry
        write_manifest(out / MANIFEST, entries)
        counts['built'] += 1; log(f'{row.key}: built {entry[0]} x{factor} -> {result.width}x{result.height}')
    log('{built} {verb}, {current} current, {skipped} skipped, {failed} failed'.format(verb='to build' if dry_run else 'built', **counts))
    return counts['failed']


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    b = sub.add_parser('build', help='upscale the pictures of a harvest folder into a pack folder')
    b.add_argument('--harvest', type=Path, required=True, help='the folder BOTW_TEXPACK_HARVEST wrote (harvest.tsv and the PNGs)')
    b.add_argument('--out', type=Path, required=True, help='the pack folder (BOTW_TEXPACK_DIR)')
    b.add_argument('--upscaler', type=Path, help='the realesrgan-ncnn-vulkan program')
    b.add_argument('--models', type=Path, help='the folder of the models (<name>.param and <name>.bin)')
    b.add_argument('--overrides', type=Path, help='overrides tsv (default: overrides.tsv in the harvest folder)')
    b.add_argument('--force', action='store_true', help='build the pictures again whose manifest entry is current')
    b.add_argument('--only', help='only these keys, comma separated')
    b.add_argument('--dry-run', action='store_true', help='print what would be built; run nothing and write nothing')
    args = parser.parse_args()
    if not args.dry_run and not (args.upscaler and args.models):
        parser.error('--upscaler and --models are required (except with --dry-run)')
    TOOL.update(upscaler=args.upscaler, models=args.models)
    try:
        failed = build(args.harvest, args.out, overrides=args.overrides, force=args.force, only=args.only.split(',') if args.only else None, dry_run=args.dry_run)
    except (TexpackError, OSError) as error:
        sys.exit(f'botw_texpack: {error}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
