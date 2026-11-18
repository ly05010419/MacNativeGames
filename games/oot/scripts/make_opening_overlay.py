#!/usr/bin/env python3
"""User-requested automatic opening skip; derive an overlay from verified originals.

The Kokiri movie has no skip UI. Generate a one-keyframe WebM without re-encoding
and shorten its BFEVFL timeline duration from 9255 frames to one frame, retaining
the event's actors/actions and normal end processing. Original inputs, event
flags and saves remain intact. Generated game content stays outside the checkout.

Timeline layout reference (facts only): zeldamods/evfl, evfl/timeline.py,
Timeline._do_read: TLIN + 0x10 is the duration and +0x20 the name pointer.
"""
import argparse
import compression.zstd as zstd
import hashlib
import json
from pathlib import Path
import struct
from make_romfs_patches import sarc_members
import webm_one_frame

EVENT = 'Event/EventFlow/Dm_Kokiri_0000.bfevfl.zs'
DICTIONARIES = 'Pack/ZsDic.pack.zs'
MOVIE = 'Event/Movie/Dm_Kokiri_0000.webm'
MOVIE_SHA = '52c062a70ca69d1a482a4a6aaed5b0fd185cba974fa405862b53076158a4ed88'
FIRST_PACKET_SHA = 'd4552a91945ae3f360e91456bf7f86fa745f1b6059543e5b99f331dde19be7e5'
# The one-keyframe WebM the game played, as ffmpeg's bit-exact remux made it (webm_one_frame.py writes the same bytes).
MOVIE_ONE_FRAME_SHA = 'cdd51e81a50fdef8e2c94ab77ec0b035be5b8522d18304621dc90022aea10048'
HASHES = {
    EVENT: 'f0df23a290c1088c9893d116e0704baa4f7c35603caf5b1c3279aaa38537cad1',
    DICTIONARIES: '1f35801d24503905093bb2f6634e5779c1413ca7e6756421160e44d996e60904',
}


def generate(romfs, output):
    romfs, output = Path(romfs), Path(output)
    repo = Path(__file__).resolve().parents[3]
    if (repo / '.git').exists() and (output.resolve() == repo or repo in output.resolve().parents):
        raise ValueError('generated opening assets must remain outside the source checkout')
    files = {f['path']: f for f in json.loads((romfs / 'index.json').read_text())['files']}
    originals = {}
    with (romfs / 'romfs.bin').open('rb') as image:
        for name, digest in HASHES.items():
            f = files[name]
            image.seek(f['offset'])
            raw = image.read(f['size'])
            if hashlib.sha256(raw).hexdigest() != digest:
                raise ValueError('unsupported opening input: ' + name)
            originals[name] = raw
        movie = files[MOVIE]
        image.seek(movie['offset'])
        digest = hashlib.sha256()
        left = movie['size']
        while left:
            block = image.read(min(left, 8 << 20))
            if not block:
                raise ValueError('truncated original opening movie')
            digest.update(block)
            left -= len(block)
        if digest.hexdigest() != MOVIE_SHA:
            raise ValueError('unsupported original opening movie')
    dictionaries = zstd.decompress(originals[DICTIONARIES])
    wanted = zstd.get_frame_info(originals[EVENT]).dictionary_id
    selected = []
    for name, (lo, hi) in sarc_members(dictionaries).items():
        if not name.endswith('.zsdic'):
            continue
        dictionary = zstd.ZstdDict(dictionaries[lo:hi])
        if dictionary.dict_id == wanted:
            selected.append(dictionary)
    if len(selected) != 1:
        raise ValueError('opening compression dictionary is not unique')
    dictionary = selected[0]
    event = zstd.decompress(originals[EVENT], zstd_dict=dictionary)
    if event[:6] != b'BFEVFL' or event.count(b'TLIN') != 1:
        raise ValueError('unexpected opening event structure')
    timeline = event.index(b'TLIN')
    name_at = struct.unpack_from('<Q', event, timeline + 0x20)[0]
    size = struct.unpack_from('<H', event, name_at)[0]
    if event[name_at + 2:name_at + 2 + size] != b'Dm_Kokiri_0000':
        raise ValueError('opening timeline identity differs')
    if struct.unpack_from('<f', event, timeline + 0x10)[0] != 9255.0:
        raise ValueError('original opening duration differs')
    changed = bytearray(event)
    struct.pack_into('<f', changed, timeline + 0x10, 1.0)
    # All relocation offsets, actors, actions and parameters remain byte-identical.
    restored = bytearray(changed)
    restored[timeline + 0x10:timeline + 0x14] = event[timeline + 0x10:timeline + 0x14]
    if restored != event:
        raise ValueError('unrelated event bytes changed')
    compressed = zstd.compress(changed, zstd_dict=dictionary)
    if zstd.decompress(compressed, zstd_dict=dictionary) != changed:
        raise ValueError('opening overlay round trip failed')
    destination = output / EVENT
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(compressed)
    digest = hashlib.sha256(compressed).hexdigest()
    # The first AV1 keyframe in a WebM of its own, not re-encoded (webm_one_frame.py; ffmpeg made it until 2026-11-12).
    with (romfs / 'romfs.bin').open('rb') as image:
        image.seek(movie['offset'])
        head = image.read(min(movie['size'], webm_one_frame.HEAD_BYTES))
    short = webm_one_frame.one_frame(webm_one_frame.read_original(head))
    # Read back on its own: one original-size AV1 keyframe, the original packet, a 16 ms movie, ffmpeg's bytes.
    probe = webm_one_frame.inspect(short)
    blocks = probe['blocks']
    if probe['tracks'] != [{'codec': 'V_AV1', 'width': 1920, 'height': 1080}] or len(blocks) != 1 or not blocks[0]['keyframe']:
        raise ValueError('short opening movie is not one original-size AV1 keyframe')
    if not 0 < probe['duration'] < 0.034:
        raise ValueError('short opening movie has an unexpected duration')
    if hashlib.sha256(blocks[0]['payload']).hexdigest() != FIRST_PACKET_SHA:
        raise ValueError('short opening movie changed the original AV1 packet')
    clip_sha = hashlib.sha256(short).hexdigest()
    if clip_sha != MOVIE_ONE_FRAME_SHA:
        raise ValueError('short opening movie differs from the one played in the game')
    clip = output / MOVIE
    clip.parent.mkdir(parents=True, exist_ok=True)
    clip.write_bytes(short)
    (output / 'manifest.tsv').write_text('# path\tsize\tsha256\toriginal_sha256\n'
        f'{EVENT}\t{len(compressed)}\t{digest}\t{HASHES[EVENT]}\n'
        f'{MOVIE}\t{clip.stat().st_size}\t{clip_sha}\t{MOVIE_SHA}\n')
    (output / 'generation.json').write_text(json.dumps(dict(
        event=EVENT, original_event_sha256=HASHES[EVENT], original_movie_sha256=MOVIE_SHA,
        original_duration_frames=9255, duration_frames=1, original_packet_sha256=FIRST_PACKET_SHA,
        movie_writer='webm_one_frame.py', movie_writer_sha256=hashlib.sha256(Path(webm_one_frame.__file__).read_bytes()).hexdigest(),
        generated_event_sha256=digest, generated_movie_sha256=clip_sha,
        probe=dict(tracks=probe['tracks'], blocks=len(blocks), duration=probe['duration'])), indent=2) + '\n')
    print('Verified opening overlay: 1-frame timeline and 1 original AV1 keyframe, no re-encoding')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--romfs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    generate(args.romfs, args.output)
