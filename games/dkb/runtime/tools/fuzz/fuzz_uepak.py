#!/usr/bin/env python3
"""Fuzz dm/gfx/tools/uepak.py's readers of Unreal Engine files: Pak (the footer, a version 11 index with its encoded entries and directory index, a legacy version 4 index) and
Pak.read of every file it lists, archive / lz4_shaders / decoded_archive (a version 2 shader code archive) and lslg (a decoded shader record).
Seeds: a version 11 pak (a stored, a Zlib and an LZ4 file), a version 4 pak (a stored and a Zlib file), a shader code archive with a stored and an LZ4-compressed LSLG record,
and an LSLG record. Index encryption is left out (no key is given: an encrypted index is the ValueError Pak raises for it; the AES path uses macOS's CommonCrypto).
Clean rejection: ValueError, which every reader raises for what it does not read, KeyError for a path the index does not list, zlib.error and LZ4BlockError from a block that
does not decode, and lslg's None. Any other exception (struct.error from a read past the end of a table, for one) and an oversized allocation are findings. Usage: see fuzzlib.py"""
import struct
import sys
import zlib

import fuzzlib
import lz4.block
import uepak

HARNESS = 'uepak'
fuzzlib.guard(uepak)


def fstr(s): b = s.encode() + b'\0'; return struct.pack('<i', len(b)) + b


def lslg(tag=b'SV'):
    """'LSLG', a stage tag, a header with a source name, u32 code length, the code area, u32 control length, the control section (+0x704 the code length), a short tail"""
    code = uepak.struct.pack('<I', 0x12345678) + bytes(0x2c) + struct.pack('<I', 0x461) + bytes(0x4c) + bytes(0x40)
    ctl = bytearray(0x880); ctl[:4] = bytes.fromhex('34127698'); struct.pack_into('<I', ctl, 0x704, len(code))
    return b'LSLG' + tag + b'\0\0FuzzShader\0\0' + struct.pack('<I', len(code)) + code + struct.pack('<I', len(ctl)) + bytes(ctl) + bytes(24)


def archive():
    raw = [lslg(b'SV'), lslg(b'SP')]
    stored = [raw[0], lz4.block.compress(raw[1], store_size=False)]
    d = struct.pack('<I', 2) + struct.pack('<I', 1) + bytes(20) + struct.pack('<I', 2) + bytes(40)       # version, map hashes, shader hashes
    d += struct.pack('<I', 1) + struct.pack('<IIII', 0, 2, 0, 0)                                          # shader map entries
    d += struct.pack('<I', 2); off = 0
    for r, s in zip(raw, stored): d += struct.pack('<QIIB', off, len(s), len(r), 0); off += len(s)       # shader entries
    d += struct.pack('<I', 0) + struct.pack('<I', 2) + struct.pack('<II', 0, 1)                           # no preload entries; shader indices
    return d + b''.join(stored)


def pak11(files):
    """Version 11: the data (each file behind a 53-byte FPakEntry, a compressed one's block table after it), the index, the directory index, the footer (magic 0xcc from the end)"""
    data, enc_entries, dirs = bytearray(), bytearray(), []
    for name, raw, meth in files:
        stored = {0: raw, 1: zlib.compress(raw), 2: lz4.block.compress(raw, store_size=False)}[meth]
        at = len(data); head = 53 + (4 + 16 if meth else 0); data += bytes(head) + stored
        v = 1 << 6 | 1 << 29 | 1 << 30 | 1 << 31 | meth << 23   # one block, sizes and offset in 32 bits
        dirs.append((name, len(enc_entries)))
        enc_entries += struct.pack('<III', v, at, len(raw)) + (struct.pack('<I', len(stored)) if meth else b'')
    ioff = len(data)
    dirindex = struct.pack('<i', 1) + fstr('/') + struct.pack('<i', len(dirs)) + b''.join(fstr(n) + struct.pack('<i', o) for n, o in dirs)
    index = fstr('../../../') + struct.pack('<i', len(files)) + struct.pack('<Q', 0) + struct.pack('<i', 0) + struct.pack('<i', 1)
    doff = ioff + len(index) + 8 + 8 + 20 + 4 + len(enc_entries) + 4
    index += struct.pack('<QQ', doff, len(dirindex)) + bytes(20) + struct.pack('<i', len(enc_entries)) + bytes(enc_entries) + struct.pack('<i', 0)
    methods = b''.join(m.ljust(32, b'\0') for m in (b'Zlib', b'LZ4', b'', b'', b''))
    foot = bytes(16) + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 11, ioff, len(index)) + bytes(20) + methods
    return bytes(data) + index + dirindex + bytes(4) + foot   # 4 spare bytes before the GUID: the reader takes back + 21 bytes


def pak4(files):
    """Legacy version 4: the data, then per file its name and FPakEntry (blocks from the file start), then the 0x2d-byte footer"""
    data, index = bytearray(), fstr('../../../') + struct.pack('<i', len(files))
    for name, raw, meth in files:
        stored = zlib.compress(raw) if meth else raw
        at = len(data); head = 53 + (4 + 16 if meth else 0); data += bytes(head) + stored
        index += fstr(name) + struct.pack('<qqqi', at, len(stored), len(raw), meth) + bytes(20)
        if meth: index += struct.pack('<i', 1) + struct.pack('<qq', at + head, at + head + len(stored))
        index += struct.pack('<BI', 0, 0x10000)
    return bytes(data) + index + b'\0' + struct.pack('<IIQQ', uepak.PAK_MAGIC, 4, len(data), len(index)) + bytes(20)


def seeds():
    body = b'fuzz file body ' * 20
    return {'pak11': pak11([('Content/a.txt', body, 0), ('Content/z.bin', body * 3, 1), ('Content/l.bin', body * 2, 2)]),
            'pak4': pak4([('Engine/a.txt', body, 0), ('Engine/b.bin', body * 2, 1)]), 'archive': archive(), 'lslg': lslg()}


_file = fuzzlib.InputFile('pakchunk0-Switch.pak')
CLEAN = (ValueError, KeyError, zlib.error, lz4.block.LZ4BlockError)


def check(data):
    try:
        if data[:4] == b'LSLG': uepak.lslg(data); return
        if data[:4] == struct.pack('<I', 2):
            a = uepak.archive(data); raws = uepak.lz4_shaders(data, a)
            if raws is not None:
                uepak.decoded_archive(data, raws)
                for r in raws: uepak.lslg(r)
            return
        p = object.__new__(uepak.Pak)   # built in two steps so its file is closed when __init__ fails too
        try:
            p.__init__(_file.write(data))
            for path in list(p.files): uepak.lslg(p.read(path))
        finally:
            if hasattr(p, 'f'): p.f.close()
    except CLEAN: return


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
