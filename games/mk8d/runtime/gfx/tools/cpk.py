#!/usr/bin/env python3
# CRI CPK archives (CRIWARE File System): Bayonetta 2 keeps all its data but sound and movies in six of them (data000.cpk …). A CPK starts with 'CPK ' and a 0x10-byte header, then an
# @UTF table that names where its table of contents is (TocOffset) and where the file data starts (ContentOffset); the TOC ('TOC ', 0x10 bytes, @UTF) has one row per file:
# DirName, FileName, FileSize (stored), ExtractSize, FileOffset (relative to the smaller of TocOffset and ContentOffset). A file whose stored size is smaller than its size is
# CRILAYLA-compressed (crilayla). The @UTF tables may be stored masked (Unicorn Overlord; unmask). shader_split.py reads every CPK through it (cpk_jobs).
# Usage: cpk.py <file.cpk> [--extract <dir> [<name regex>]] lists (or extracts) the files.
import re, struct, sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import open
from pathlib import Path

def unmask(b):
    """An @UTF table stored masked (Unicorn Overlord's CPKs): each byte XOR the low byte of a 16-bit value that starts at 0x655f and is multiplied by 0x4115 after every byte"""
    out, m = bytearray(len(b)), 0x655f
    for i, x in enumerate(b): out[i] = x ^ (m & 0xff); m = m * 0x4115 & 0xffff
    return bytes(out)

def utf_at(d, o, n):
    """The @UTF table of n bytes at d[o:], unmasked when it is stored masked"""
    t = d[o:o + n]
    return t if t[:4] == b'@UTF' else unmask(t)

def utf(d, o=0):
    """A CRI @UTF table at d[o:]: '@UTF', u32 table length, then (offsets from o + 8, big-endian) u16 version, u16 rows offset, u32 strings offset, u32 data offset, u32 table name,
    u16 column count, u16 row width, u32 row count, then the columns: a flag byte (storage 0x10 zero, 0x30 constant, 0x50 per row; type in the low nibble: 0/1 u8/s8, 2/3 u16/s16,
    4/5 u32/s32, 6/7 u64/s64, 8 float, 0xa string, 0xb data) and the name's offset. → (table name, [row dicts])"""
    if d[o:o + 4] != b'@UTF': raise ValueError(f'no @UTF table at {o:#x}')
    _, ro, so, do, tn, nc, rw, nr = struct.unpack_from('>HHIIIHHI', d, o + 8)
    b = o + 8
    sz = lambda off: d[b + so + off:d.index(b'\0', b + so + off)].decode('utf-8', 'replace')
    fmt = {0: '>B', 1: '>b', 2: '>H', 3: '>h', 4: '>I', 5: '>i', 6: '>Q', 7: '>q', 8: '>f'}
    def val(t, p):
        if t in fmt: return struct.unpack_from(fmt[t], d, p)[0], struct.calcsize(fmt[t])
        if t == 0xa: return sz(struct.unpack_from('>I', d, p)[0]), 4
        if t == 0xb:
            a, n = struct.unpack_from('>II', d, p)
            return d[b + do + a:b + do + a + n], 8
        raise ValueError(f'@UTF column type {t:#x}')
    cols, p = [], o + 0x20
    for _ in range(nc):
        fl = d[p]; name = sz(struct.unpack_from('>I', d, p + 1)[0]); p += 5
        st, t = fl & 0xf0, fl & 0x0f
        if st == 0x30:
            v, n = val(t, p); p += n; cols.append((name, 'c', v))
        elif st == 0x50: cols.append((name, 'r', t))
        else: cols.append((name, 'z', None))
    rows = []
    for i in range(nr):
        p, r = b + ro + i * rw, {}
        for name, st, x in cols:
            if st == 'r': r[name], n = val(x, p); p += n
            elif st == 'c': r[name] = x
            else: r[name] = None
        rows.append(r)
    return sz(tn), rows

def files(path):
    """→ [(path in the archive, offset, stored length, length)] of a CPK; a file with both a TOC and an ITOC only by its TOC (Bayonetta 2's have names)"""
    with open(path, 'rb') as f:
        h = f.read(0x800)
        if h[:4] != b'CPK ': raise ValueError(f'{path}: not a CPK')
        n = struct.unpack_from('<I', h, 8)[0]
        if 0x10 + n > len(h): f.seek(0); h = f.read(0x10 + n)
        _, (hd,) = utf(utf_at(h, 0x10, n))
        toc, ctt = hd.get('TocOffset'), hd.get('ContentOffset')
        if not toc: raise ValueError(f'{path}: no TOC (ITOC only is not read)')
        f.seek(toc); t = f.read(0x10)
        if t[:4] != b'TOC ': raise ValueError(f'{path}: no TOC at {toc:#x}')
        n = struct.unpack_from('<I', t, 8)[0]; f.seek(toc + 0x10); t = utf_at(f.read(n), 0, n)
    base = min(toc, ctt) if ctt else toc
    out = []
    for r in utf(t)[1]:
        nm = (r['DirName'] + '/' if r.get('DirName') else '') + r['FileName']
        out.append((nm, base + r['FileOffset'], r['FileSize'], r['ExtractSize'] if r.get('ExtractSize') is not None else r['FileSize']))
    return out

_BITS = [format(i, '08b') for i in range(256)]
def crilayla(c):
    """CRILAYLA: 'CRILAYLA', u32 size, u32 compressed length, then the compressed bits, then 0x100 bytes stored as they are (the output's first 0x100). The bits are read from the end of the
    compressed bytes backwards, highest bit first, and the output is written from its end backwards: a 1 bit, 13 offset bits (+3, forward from the write position) and a length in growing
    fields (2, 3, 5 bits, then 8 bits while all ones; +3), or a 0 bit and a literal byte. The bits are read from a string of '0' / '1' (twice as fast as shifting in Python: 29 MB/s on
    Bayonetta 2's shader.dat)"""
    if c[:8] != b'CRILAYLA': raise ValueError('not CRILAYLA')
    usz, csz = struct.unpack_from('<II', c, 8)
    out = bytearray(usz + 0x100)
    out[:0x100] = c[0x10 + csz:0x10 + csz + 0x100]
    s = ''.join(map(_BITS.__getitem__, c[0x10 + csz - 1:0xf:-1]))
    p, w = 0, len(out) - 1
    while w >= 0x100:
        if s[p] == '1':
            back = int(s[p + 1:p + 14], 2) + 3
            v = int(s[p + 14:p + 16], 2); p += 16; ln = 3 + v
            if v == 3:
                v = int(s[p:p + 3], 2); p += 3; ln += v
                if v == 7:
                    v = int(s[p:p + 5], 2); p += 5; ln += v
                    if v == 31:
                        while True:
                            v = int(s[p:p + 8], 2); p += 8; ln += v
                            if v != 255: break
            if back >= ln:
                out[w - ln + 1:w + 1] = out[w + back - ln + 1:w + back + 1]; w -= ln
            else:
                for _ in range(ln): out[w] = out[w + back]; w -= 1
        else:
            out[w] = int(s[p + 1:p + 9], 2); p += 9; w -= 1
    return bytes(out)

def read(path, ent):
    nm, off, csz, usz = ent
    with open(path, 'rb') as f:
        f.seek(off); c = f.read(csz)
    if csz != usz or c[:8] == b'CRILAYLA':
        c = crilayla(c)
    if len(c) != usz: raise ValueError(f'{path}#{nm}: {len(c)} bytes, the TOC says {usz}')
    return c

if __name__ == '__main__':
    p = sys.argv[1]
    es = files(p)
    if '--extract' in sys.argv:
        i = sys.argv.index('--extract'); out = Path(sys.argv[i + 1]); rx = re.compile(sys.argv[i + 2]) if len(sys.argv) > i + 2 else None
        for e in es:
            if rx and not rx.search(e[0]): continue
            dst = out / e[0]; dst.parent.mkdir(parents=True, exist_ok=True); dst.write_bytes(read(p, e))
    else:
        for nm, off, csz, usz in es: print(f'{nm}\t{off:#x}\t{csz}\t{usz}')
