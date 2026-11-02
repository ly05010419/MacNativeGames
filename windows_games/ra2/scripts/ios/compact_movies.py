#!/usr/bin/env python3
"""Make compact copies of the MIX files that hold movies, for a phone.

    python3 -I scripts/ios/compact_movies.py <game dir> <out dir> [--jobs N]

Red Alert 2's movies (the campaigns' briefings and cut scenes, about 1 GB
of Bink video at 2 Mbit/s) are most of what a phone stores. Each MIX file
of the install that holds Bink movies is written to <out dir> under its
own name with every movie re-encoded as a compact movie (binkw32.cpp,
"XRMV": the Bink header the game reads, then VP9 and Opus in Matroska),
everything else copied as it is; the new MIX has a plain (unencrypted)
header, which the game reads too. The frames are the ones binkw32 shows
(tools/bink_decode, decoding from black as RAD's decoder does), so only
the codec differs: about a quarter of the size, at SSIM 0.96 against the
Bink frames. The Mac keeps the original files.

Movies are cached in <out dir>/cache by the SHA-256 of their bytes and the
encoder settings, so running it again only writes MIX files whose source
changed. Needs ffmpeg with libvpx and libopus (brew install ffmpeg) and
bink_decode from the Mac build (scripts/build.sh; BUILD_DIR as there, or
BINK_DECODE=<path>). Everything it writes is made from your own files and
stays on your Mac and phone.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # nothing written into the source folder
sys.path.insert(0, str(ROOT / 'scripts'))
import mixls  # noqa: E402

MAGIC = b'XRMV'
# Changing these makes every movie again (they are part of the cache key).
VIDEO = ['-c:v', 'libvpx-vp9', '-crf', '32', '-b:v', '0', '-row-mt', '1', '-cpu-used', '4',
         '-threads', '2']
AUDIO = ['-c:a', 'libopus', '-b:a', '64k']
SETTINGS = 'v1 ' + ' '.join(VIDEO + AUDIO)


def bink_decode_path():
    if p := os.environ.get('BINK_DECODE'):
        return Path(p)
    build = Path(os.environ.get('BUILD_DIR', ROOT / 'build/main'))
    return build / 'x86recomp/tools/bink_decode/bink_decode'


def bink_header_len(bik):
    """The Bink header with its audio track tables (what binkw32 reads):
    44 bytes, per track 4 + 4, from revision 'k' 4 more, then the IDs."""
    tracks = struct.unpack_from('<I', bik, 40)[0]
    return 44 + 8 * tracks + (4 if bik[3] >= ord('k') else 0) + 4 * tracks


def encode(bik, decoder, work):
    """One Bink movie as a compact movie (bytes)."""
    src = work / 'in.bik'
    dst = work / 'out.mkv'
    src.write_bytes(bik)
    dec = subprocess.Popen([decoder, src], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    info = dec.stderr.readline().split()
    if len(info) != 5:
        dec.kill()
        raise RuntimeError(f'bink_decode: {dec.stderr.read().decode().strip() or "no header"}')
    w, h, frames, rate, div = map(int, info)
    enc = subprocess.run(
        ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-y',
         '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-s', f'{w}x{h}', '-framerate', f'{rate}/{div}',
         '-i', '-', '-i', src, '-map', '0:v', '-map', '1:a?', *VIDEO, *AUDIO, '-f', 'matroska',
         dst], stdin=dec.stdout, capture_output=True, text=True)
    dec.stdout.close()
    err = dec.stderr.read().decode().strip()
    if dec.wait() != 0:
        raise RuntimeError(f'bink_decode: {err}')
    if enc.returncode != 0:
        raise RuntimeError(f'ffmpeg: {enc.stderr.strip()}')
    count = subprocess.run(
        ['ffprobe', '-v', 'error', '-count_packets', '-select_streams', 'v:0', '-show_entries',
         'stream=nb_read_packets', '-of', 'csv=p=0', dst], capture_output=True, text=True,
        check=True).stdout.strip()
    if int(count or 0) != frames:
        raise RuntimeError(f'{count} frames encoded, the movie has {frames}')
    header = bik[:bink_header_len(bik)]
    rest = struct.pack('<I', len(header)) + header + dst.read_bytes()
    return MAGIC + struct.pack('<I', len(rest)) + rest


def compact_movie(bik, decoder, cache):
    key = hashlib.sha256(SETTINGS.encode() + b'\0' + bik).hexdigest()
    path = cache / f'{key}.xrmv'
    if path.is_file():
        return path.read_bytes()
    with tempfile.TemporaryDirectory(dir=cache) as t:
        data = encode(bik, decoder, Path(t))
        tmp = Path(t) / 'movie'
        tmp.write_bytes(data)
        tmp.replace(path)
    return data


def write_mix(path, entries):
    """A MIX file with a plain header (no checksum, not encrypted): flags 0,
    the entry count, the body size, (id, offset, size) per entry in the
    order given (the source's, sorted as the game searches it), the body."""
    body_size = sum(len(d) for _, d in entries)
    table, off = bytearray(), 0
    for id_, d in entries:
        table += struct.pack('<III', id_, off, len(d))
        off += len(d)
    tmp = path.with_name(path.name + '.part')
    with open(tmp, 'wb') as f:
        f.write(struct.pack('<IHI', 0, len(entries), body_size))
        f.write(table)
        for _, d in entries:
            f.write(d)
    tmp.replace(path)


def compact_mix(src, out, key, decoder, cache, jobs):
    """Writes out/<src name> when src holds Bink movies; returns its path,
    or None for a MIX without movies."""
    data = src.read_bytes()
    try:
        ents = mixls.parse(data, key)
    except Exception:
        return None  # not a MIX file this reads
    movies = [i for i, (o, s) in ents.items() if data[o:o + 3] == b'BIK']
    if not movies:
        return None
    dest = out / src.name
    stamp = out / (src.name + '.json')
    want = {'source_size': len(data), 'source_sha256': hashlib.sha256(data).hexdigest(),
            'settings': SETTINGS}
    if dest.is_file() and stamp.is_file() and json.loads(stamp.read_text()) == want:
        print(f'  {src.name}: up to date ({dest.stat().st_size >> 20} MB)')
        return dest
    print(f'  {src.name}: {len(movies)} movies, {sum(ents[i][1] for i in movies) >> 20} MB')
    with ThreadPoolExecutor(jobs) as pool:
        done = dict(zip(movies, pool.map(
            lambda i: compact_movie(data[ents[i][0]:ents[i][0] + ents[i][1]], decoder, cache),
            movies)))
    entries = [(i, done[i] if i in done else data[o:o + s]) for i, (o, s) in ents.items()]
    write_mix(dest, entries)
    stamp.write_text(json.dumps(want))
    print(f'  {src.name}: {len(data) >> 20} MB -> {dest.stat().st_size >> 20} MB')
    return dest


def compact_install(game, out, jobs=3):
    """Compact MIX files for the install's movie archives, by lower-case
    file name."""
    decoder = bink_decode_path()
    if not decoder.is_file():
        sys.exit(f'{decoder} not found: build the Mac version first (scripts/build.sh) '
                 'or set BINK_DECODE')
    out.mkdir(parents=True, exist_ok=True)
    cache = out / 'cache'
    cache.mkdir(exist_ok=True)
    key = mixls.public_key(game)
    made = {}
    for f in sorted(game.iterdir()):
        if f.is_file() and f.suffix.lower() == '.mix':
            if p := compact_mix(f, out, key, decoder, cache, jobs):
                made[f.name.lower()] = p
    return made


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('game', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--jobs', type=int, default=3)
    a = ap.parse_args()
    for name, p in compact_install(a.game, a.out, a.jobs).items():
        print(f'{name}: {p}')


if __name__ == '__main__':
    main()
