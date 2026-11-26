#!/usr/bin/env python3
"""Extract verified, shader-named ITT Pak resources; not a full shader scanner.

Uses an explicitly supplied Oodle decoder, never downloads one. Only reads the
bundled RomFS image. Output must be new, and never touches runtime profiles.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import tempfile
from scan_shader_resources import scan

MAX_RESOURCE = 256 << 20


def extract_entry(read, entry, decode):
    size, packed = entry['uncompressed'], entry['compressed']
    if entry['encrypted'] or not 0 < size <= MAX_RESOURCE or not 0 < packed <= MAX_RESOURCE:
        raise ValueError('encrypted or unbounded resource')
    offset = entry['offset']
    prefix = read(offset, 48)
    if len(prefix) != 48 or struct.unpack_from('<QQQI', prefix) != (0, packed, size, entry['method']):
        raise ValueError('stored resource metadata mismatch')
    if not entry['method']:
        tail = read(offset+48, 5)
        flags, blocksize = struct.unpack('<BI', tail)
        if flags or entry['blocks'] or blocksize:
            raise ValueError('unexpected uncompressed block metadata')
        payload = read(offset+53, packed)
        if size != packed or hashlib.sha1(payload).digest() != prefix[28:48]:
            raise ValueError('resource SHA1/size mismatch')
        return payload
    count = struct.unpack('<I', read(offset+48, 4))[0]
    if not 0 < count <= 65536 or count != entry['blocks']:
        raise ValueError('invalid resource block count')
    table = read(offset+52, 16*count+5)
    flags, blocksize = struct.unpack_from('<BI', table, 16*count)
    if flags or blocksize != entry['block_size'] or not 0 < blocksize <= MAX_RESOURCE:
        raise ValueError('invalid block metadata')
    if count != (size+blocksize-1)//blocksize:
        raise ValueError('block count/output length mismatch')
    header_size, previous, ranges = 57+16*count, 57+16*count, []
    for i in range(count):
        lo, hi = struct.unpack_from('<QQ', table, 16*i)
        if lo != previous or not lo < hi <= header_size+packed:
            raise ValueError('noncontiguous or out-of-range compressed block')
        ranges.append((lo, hi)); previous = hi
    if previous != header_size+packed:
        raise ValueError('compressed payload size mismatch')
    payload = read(offset+header_size, packed)
    if len(payload) != packed or hashlib.sha1(payload).digest() != prefix[28:48]:
        raise ValueError('compressed resource SHA1 mismatch')
    result = bytearray()
    for lo, hi in ranges:
        expected = min(blocksize, size-len(result))
        raw = decode(entry['compression'], payload[lo-header_size:hi-header_size], expected)
        if len(raw) != expected:
            raise ValueError('decompressed block size mismatch')
        result.extend(raw)
    return bytes(result)


def oodle_decoder(executable):
    def decode(method, raw, size):
        if method != 'Oodle':
            raise ValueError('unsupported compression: '+method)
        with tempfile.TemporaryDirectory(prefix='mng-oodle-') as tmp:
            src, dst = Path(tmp)/'input', Path(tmp)/'output'
            src.write_bytes(raw)
            run = subprocess.run([str(executable), str(src), '--length', str(size),
                                  '--output', str(dst), '--verbose'], capture_output=True, text=True, timeout=60)
            # CLI writes the entire output allocation: check its actual decoded
            # byte count too, rather than accepting a zero-filled short output.
            if run.returncode or not re.search(rf'^Extracted {size} bytes$', run.stdout, re.M):
                raise ValueError('Oodle decode failed: '+(run.stderr+run.stdout)[-1000:])
            return dst.read_bytes()
    return decode


def extract(index, image, output, decoder):
    if output.exists():
        raise ValueError('output must be a new directory')
    report = scan(index, image)
    pak = next(e for e in json.loads(index.read_text())['files'] if e['path']==report['pak'])
    output.mkdir(parents=True)
    records = []
    with image.open('rb') as stream:
        def read(off, n):
            if off < 0 or n < 0 or off+n > pak['size']:
                raise ValueError('read outside Pak')
            stream.seek(pak['offset']+off)
            data = stream.read(n)
            if len(data) != n: raise ValueError('short Pak read')
            return data
        for entry in report['matches']:
            if 'method' not in entry:
                raise ValueError('unencoded resource unsupported')
            raw = extract_entry(read, entry, oodle_decoder(decoder))
            # Flat, generated names avoid archive path traversal.
            name = f'{len(records):04d}-'+Path(entry['path']).name
            (output/name).write_bytes(raw)
            records.append(dict(path=entry['path'], output=name, size=len(raw), sha256=hashlib.sha256(raw).hexdigest()))
    result = dict(resources=records, decoder_sha256=hashlib.sha256(decoder.read_bytes()).hexdigest(),
                  scope='Shader-named resources only; embedded material shaders require further enumeration and parsing.')
    (output/'extraction-report.json').write_text(json.dumps(result, indent=2)+'\n')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('index', 'image', 'output', 'decoder'):
        p.add_argument('--'+name, type=Path, required=True)
    a=p.parse_args()
    print(f"Extracted {len(extract(a.index,a.image,a.output,a.decoder.resolve())['resources'])} verified resources")


if __name__ == '__main__': main()
