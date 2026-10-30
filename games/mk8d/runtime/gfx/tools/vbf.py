"""Virtuos SRYK archives (FFX/X-2 Switch): two MD5 tables, 32-byte file records, names and 64 KB zlib/raw blocks.
Read members through romfs.open so archives inside a RomFS image need no extraction. Missing/shared entries have offset UINT64_MAX.
"""
import struct
import zlib
from romfs import open


def entries(path):
    with open(path, 'rb') as f:
        head = f.read(16)
        if len(head) != 16 or head[:4] != b'SRYK': raise ValueError('not a VBF archive')
        hlen, count = struct.unpack_from('<IQ', head, 4)
        if not 16 + count * 64 + 4 <= hlen <= 256 << 20: raise ValueError('invalid VBF header length')
        head += f.read(hlen - 16)
    if len(head) != hlen: raise ValueError('truncated VBF header')
    strings = 16 + count * 64
    slen = struct.unpack_from('<I', head, strings)[0]
    blocks = strings + slen
    if slen < 4 or blocks > hlen: raise ValueError('invalid VBF string table')
    out = []
    for i in range(count):
        bi, _, size, offset, name = struct.unpack_from('<IIQQQ', head, 16 + count * 32 + i * 32)
        if offset == 0xffffffffffffffff: continue
        start = strings + 4 + name
        if not strings + 4 <= start < blocks: raise ValueError('invalid VBF name offset')
        end = head.find(b'\0', start, blocks)
        if end < 0: raise ValueError('unterminated VBF name')
        n = (size + 65535) // 65536
        if blocks + (bi + n) * 2 > hlen or offset < hlen: raise ValueError('invalid VBF block range')
        lengths = struct.unpack_from(f'<{n}H', head, blocks + bi * 2)
        out.append((head[start:end].decode('utf-8'), size, offset, lengths))
    return out


def read(f, entry):
    name, size, offset, lengths = entry
    f.seek(offset); chunks = []
    for length in lengths:
        n = min(65536, size); length = length or 65536
        data = f.read(length)
        if len(data) != length: raise ValueError(f'{name}: truncated VBF block')
        if length != n:
            dec = zlib.decompressobj()
            data = dec.decompress(data, n + 1)
            if not dec.eof or dec.unconsumed_tail: raise ValueError(f'{name}: invalid VBF compressed block')
        if len(data) != n: raise ValueError(f'{name}: VBF block size mismatch')
        chunks.append(data); size -= n
    return b''.join(chunks)
