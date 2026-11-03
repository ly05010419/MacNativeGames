"""Westwood CSF string tables (RA2.CSF): read and write.

A ' FSC' header (version, label count, string count, unused, language;
9 is Chinese, 0 US English), then for each label ' LBL' (string count,
name length, ASCII name) and its strings: ' RTS' (length in UTF-16 units,
the UTF-16LE text with every byte inverted) or 'WRTS' (the same, then an
ASCII extra value with its length, e.g. a sound name).
"""

import struct

LANGUAGE_CHINESE = 9


def read(path):
    with open(path, 'rb') as f:
        return parse(f.read())


def parse(d):
    magic, version, nlabels, _nstrings, unused, language = struct.unpack_from('<4s5I', d, 0)
    if magic != b' FSC':
        raise ValueError(f'not a CSF file (magic {magic!r})')
    pos, labels = 24, []
    for _ in range(nlabels):
        tag, count, n = struct.unpack_from('<4sII', d, pos)
        if tag != b' LBL':
            raise ValueError(f'label expected at {pos:#x}, found {tag!r}')
        pos += 12
        name = d[pos:pos + n].decode('ascii')
        pos += n
        values = []
        for _ in range(count):
            stag, slen = struct.unpack_from('<4sI', d, pos)
            if stag not in (b' RTS', b'WRTS'):
                raise ValueError(f'string expected at {pos:#x}, found {stag!r}')
            pos += 8
            text = bytes(b ^ 0xFF for b in d[pos:pos + 2 * slen]).decode('utf-16-le')
            pos += 2 * slen
            extra = None
            if stag == b'WRTS':
                (elen,) = struct.unpack_from('<I', d, pos)
                extra = d[pos + 4:pos + 4 + elen]
                pos += 4 + elen
            values.append((text, extra))
        labels.append((name, values))
    return {'version': version, 'unused': unused, 'language': language, 'labels': labels}


def serialize(t):
    out = bytearray()
    nstrings = sum(len(v) for _, v in t['labels'])
    out += struct.pack('<4s5I', b' FSC', t['version'], len(t['labels']), nstrings, t['unused'],
                       t['language'])
    for name, values in t['labels']:
        nb = name.encode('ascii')
        out += struct.pack('<4sII', b' LBL', len(values), len(nb)) + nb
        for text, extra in values:
            enc = text.encode('utf-16-le')
            out += struct.pack('<4sI', b'WRTS' if extra is not None else b' RTS', len(enc) // 2)
            out += bytes(b ^ 0xFF for b in enc)
            if extra is not None:
                out += struct.pack('<I', len(extra)) + extra
    return bytes(out)


def write(path, t):
    with open(path, 'wb') as f:
        f.write(serialize(t))
