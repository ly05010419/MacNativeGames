#!/usr/bin/env python3
"""Find the NRO modules (fighter and item code) inside Super Smash Bros. Ultimate's data.arc and verify each one.

The game loads its NROs through nn::ro from data.arc and registers their SHA-256 digests in .nrr/cross2_<version>.nrr. This
script does not parse the archive's file tables: it looks at every zstd frame (and every raw NRO header) in data.arc,
decompresses the first bytes, and keeps a module only when its SHA-256 is one of the digests the NRR lists. So nothing
unverified is written, and every listed digest must be found, or the script fails naming the missing count.

    scripts/extract_nros.py --romfs <work>/extracted/v1302/romfs --nrr .nrr/cross2_130001.nrr --output <work>/extracted/v1302/nro

Output: <sha256>.nro per module and manifest.json (digest, size, where it was found). Reads only the verified RomFS image.
"""
import argparse
import hashlib
import json
import struct
import sys
from compression import zstd
from pathlib import Path

ZSTD_MAGIC = b'\x28\xb5\x2f\xfd'
CHUNK = 256 << 20
OVERLAP = 1 << 20


def require(condition, message):
    if not condition:
        sys.exit(f'extract_nros: {message}')


def nrr_digests(image, entry):
    with image.open('rb') as f:
        f.seek(entry['offset']); nrr = f.read(entry['size'])
    require(nrr[:4] == b'NRR0', f'{entry["path"]}: not an NRR')
    offset, count = struct.unpack_from('<II', nrr, 0x340)
    require(0 < count < 4096 and offset + 32 * count <= len(nrr), f'{entry["path"]}: digest table out of bounds')
    return {nrr[offset + 32 * i:offset + 32 * i + 32].hex() for i in range(count)}


def nro_size(head):
    """The NRO's file size from its header, or None when `head` does not start an NRO."""
    if len(head) < 0x20 or head[0x10:0x14] != b'NRO0':
        return None
    size = struct.unpack_from('<I', head, 0x18)[0]
    return size if 0x80 <= size < (256 << 20) else None


def decompress_frame(view, limit=None):
    """Decompress one zstd frame from the start of `view`; with `limit`, stop after that many output bytes."""
    d = zstd.ZstdDecompressor()
    out = bytearray(); pos = 0
    while not d.eof and pos < len(view):
        step = view[pos:pos + (64 << 10)]; pos += len(step)
        out += d.decompress(step, max_length=-1 if limit is None else max(limit - len(out), 1))
        if limit is not None and len(out) >= limit:
            return bytes(out[:limit])
        while limit is None and d.needs_input is False and not d.eof:
            out += d.decompress(b'')
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--romfs', required=True, type=Path, help='folder with romfs.bin and index.json (merge_romfs.py)')
    ap.add_argument('--nrr', default='.nrr/cross2_130001.nrr', help='the NRR whose digests the modules must match')
    ap.add_argument('--output', required=True, type=Path)
    args = ap.parse_args()
    index = json.loads((args.romfs / 'index.json').read_text())
    require(index.get('status') == 'pass', 'the RomFS index is not a verified merge')
    image = args.romfs / 'romfs.bin'
    files = {e['path']: e for e in index['files']}
    require(args.nrr in files and 'data.arc' in files, f'{args.nrr} or data.arc missing from the RomFS')
    wanted = nrr_digests(image, files[args.nrr])
    arc = files['data.arc']
    args.output.mkdir(parents=True, exist_ok=True)
    found, frames, raw = {}, 0, 0
    with image.open('rb') as f:
        for start in range(0, arc['size'], CHUNK):
            f.seek(arc['offset'] + start)
            buf = f.read(min(CHUNK + OVERLAP, arc['size'] - start))
            view = memoryview(buf)
            end = min(CHUNK, len(buf))
            for magic, compressed in ((ZSTD_MAGIC, True), (b'NRO0', False)):
                p = buf.find(magic, 0)
                while 0 <= p < end:
                    at = p if compressed else p - 0x10
                    if at >= 0:
                        try:
                            head = decompress_frame(view[at:], 0x20) if compressed else bytes(view[at:at + 0x20])
                        except zstd.ZstdError:
                            head = b''
                        size = nro_size(head)
                        frames += compressed
                        if size:
                            try:
                                data = decompress_frame(view[at:]) if compressed else bytes(view[at:at + size])
                            except zstd.ZstdError:
                                data = b''
                            if not compressed: raw += 1
                            digest = hashlib.sha256(data[:size]).hexdigest() if len(data) >= size else None
                            if digest in wanted and digest not in found:
                                (args.output / f'{digest}.nro').write_bytes(data[:size])
                                found[digest] = dict(sha256=digest, size=size, arc_offset=start + at, zstd=compressed)
                                print(f'{len(found)}/{len(wanted)} {digest[:16]} {size} bytes at data.arc+{start + at:#x}', flush=True)
                    p = buf.find(magic, p + 1)
            print(f'scanned {min(start + CHUNK, arc["size"]) >> 30} / {arc["size"] >> 30} GiB, {frames} zstd frames', flush=True)
    missing = sorted(wanted - set(found))
    (args.output / 'manifest.json').write_text(json.dumps(dict(
        status='pass' if not missing else 'incomplete', nrr=args.nrr, listed=len(wanted), found=len(found), missing=missing,
        modules=sorted(found.values(), key=lambda m: m['arc_offset']),
        integrity='each module is SHA-256-equal to a digest the NRR lists'), indent=2) + '\n')
    require(not missing, f'{len(missing)} of {len(wanted)} listed modules not found in data.arc')
    print(json.dumps(dict(status='pass', modules=len(found), zstd_frames=frames)))


if __name__ == '__main__':
    main()
