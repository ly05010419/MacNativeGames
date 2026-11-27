#!/usr/bin/env python3
"""Read-only ITT Unreal Pak v11 shader inventory, without booting the game.

Reads the existing RomFS index and the bundled image through the flat work symlink.
Verifies primary/full-directory index SHA1 and encoded shader-entry metadata. Does
not decrypt, decompress Oodle, guess NVN control bytes, or claim complete coverage.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

MAGIC = 0x5A6F12E1
MAX_INDEX = 128 << 20


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def take(self, n):
        if n < 0 or n > len(self.data) - self.at:
            raise ValueError('truncated archive metadata')
        result = self.data[self.at:self.at+n]
        self.at += n
        return result

    def unpack(self, fmt):
        values = struct.unpack(fmt, self.take(struct.calcsize(fmt)))
        return values[0] if len(values) == 1 else values

    def string(self):
        n = self.unpack('<i')
        if not 0 < n <= 65536:
            raise ValueError('unsupported FString length/encoding')
        raw = self.take(n)
        if raw[-1] != 0:
            raise ValueError('unterminated FString')
        return raw[:-1].decode('utf-8')


def decode_entry(encoded, location, methods):
    if not 0 <= location < len(encoded):
        raise ValueError('unencoded/invalid entry location')
    r = Reader(encoded[location:])
    flags = r.unpack('<I')
    block_size = flags & 63
    block_size = r.unpack('<I') if block_size == 63 else block_size << 11
    method = (flags >> 23) & 63
    offset = r.unpack('<I' if flags & (1 << 31) else '<Q')
    unpacked = r.unpack('<I' if flags & (1 << 30) else '<Q')
    packed = r.unpack('<I' if flags & (1 << 29) else '<Q') if method else unpacked
    if method > len(methods):
        raise ValueError('unknown compression method')
    return dict(offset=offset, uncompressed=unpacked, compressed=packed,
                compression=methods[method-1] if method else 'None',
                method=method, encrypted=bool(flags & (1 << 22)),
                blocks=(flags >> 6) & 65535, block_size=block_size)


def scan(index, image):
    files = json.loads(index.read_text())['files']
    pak = next(x for x in files if x['path'] == 'Nuts/Content/Paks/Nuts-Switch.pak')
    start, size = pak['offset'], pak['size']
    if start < 0 or size < 221 or start + size > image.stat().st_size:
        raise ValueError('Pak outside RomFS')
    with image.open('rb') as stream:
        def read(off, n):
            if off < 0 or n < 0 or off + n > size:
                raise ValueError('metadata outside Pak')
            stream.seek(start + off)
            data = stream.read(n)
            if len(data) != n:
                raise ValueError('short Pak read')
            return data

        def verified(off, n, digest):
            if n > MAX_INDEX:
                raise ValueError('index exceeds bounded reader limit')
            data = read(off, n)
            if hashlib.sha1(data).digest() != digest:
                raise ValueError('Pak index SHA1 mismatch')
            return data

        footer = Reader(read(size - 221, 221))
        footer.take(16)  # key GUID, never emitted
        if footer.unpack('<B'):
            raise ValueError('encrypted Pak index is not supported')
        if footer.unpack('<II') != (MAGIC, 11):
            raise ValueError('expected ITT Pak version 11')
        off, length = footer.unpack('<QQ')
        digest = footer.take(20)
        methods = [footer.take(32).split(b'\0', 1)[0].decode('ascii') for _ in range(5)]
        primary = Reader(verified(off, length, digest))
        mount = primary.string()
        count = primary.unpack('<i')
        if not 0 <= count <= 2000000:
            raise ValueError('invalid Pak entry count')
        primary.take(8)  # path-hash seed
        secondary = {}
        for kind in ('path_hash', 'directory'):
            present = primary.unpack('<I')
            if present not in (0, 1):
                raise ValueError('invalid secondary index flag')
            if present:
                a, n = primary.unpack('<QQ')
                secondary[kind] = (a, n, primary.take(20))
        encoded = primary.take(primary.unpack('<i'))
        if 'directory' not in secondary:
            raise ValueError('full directory index absent')
        directory = Reader(verified(*secondary['directory']))
        nd = directory.unpack('<i')
        if not 0 <= nd <= count:
            raise ValueError('invalid directory count')
        names, found = set(), []
        for _ in range(nd):
            prefix = directory.string()
            nf = directory.unpack('<i')
            if not 0 <= nf <= count - len(names):
                raise ValueError('invalid directory file count')
            for _ in range(nf):
                path, location = prefix + directory.string(), directory.unpack('<i')
                if path in names:
                    raise ValueError('duplicate Pak path')
                names.add(path)
                if 'shader' not in path.lower() and 'pipelinecache' not in path.lower():
                    continue
                item = dict(path=path, encoded_location=location)
                if location >= 0:
                    entry = decode_entry(encoded, location, methods)
                    # Stored FPakEntry offset is zero; sizes/method must agree.
                    header = Reader(read(entry['offset'], 28))
                    stored_offset, packed, unpacked, method = header.unpack('<QQQI')
                    if stored_offset != 0 or (packed, unpacked, method) != (entry['compressed'], entry['uncompressed'], entry['method']):
                        raise ValueError('encoded/stored entry metadata mismatch')
                    item.update(entry)
                found.append(item)
        if len(names) != count or directory.at != len(directory.data):
            raise ValueError('directory count/length mismatch')
    return dict(pak=pak['path'], version=11, mount=mount, files=count,
                verified_indices=['primary', 'full_directory'], matches=found,
                scope='Filename inventory only; material-embedded shader maps are not enumerated. No extraction or compilation performed.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--index', type=Path, required=True)
    p.add_argument('--image', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if args.output.resolve() in (args.index.resolve(), args.image.resolve()):
        p.error('output must not replace an input')
    result = scan(args.index, args.image)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f"Verified {result['files']} Pak entries; {len(result['matches'])} shader-named resources; no game launch")


if __name__ == '__main__':
    main()
