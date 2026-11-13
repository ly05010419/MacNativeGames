#!/usr/bin/env python3
"""Camelot's archive (Mario Tennis Fever): the whole game in one RomFS file, rom:/42, magic 'CRA.', its index built into the game's code.
The file starts with its own table of chunks (u32 'CRA.', u16 0x200, u16 0x100, u32 0x2a0, u32 length, then 16-byte rows); nothing here needs it, as the game's NRO lists the files itself:
  · a file table in read-only data, found by the code that reads it (index_of): one i16 per file, then records at table + i16[i] + i × 18 + 2 × count, each:
    +0 u16 how far back the record holding the path's prefix is, +2 u8 the prefix's length, +3 i8 a suffix's index (-1 none), +4 u32 size ^ 0xff00ff00, +8 u40 offset ^ 0xff00ff00ff, +0xd the rest of the
    path up to a zero. A character is stored as (i - c) & 0x7f, i the index of the record holding it; a prefix may lie in several records (name). A '$' in the path stands for the folder holding it, so
    shader/font/$ is shader/font/font. The suffixes are a table of pointers in the data segment.
  · a checksum per file: u32[i] after the records, the high and low word of the sum of a word of each 16-byte block, xored; read() checks it, so a wrong key or layout never passes silently.
Each file is scrambled in 16-byte blocks at its own offset, with a key from the first 4 characters of its last path element, up to its first '.' or '_' (key: the game mixes its read state into two CRCs
whose terms cancel, as CRC is affine, so the state can be taken as 0). The key's low 3 bits choose one of 8 variants (unscramble), as nro0+0x1196824 on 1.1.1 does.
Most files are NARC containers (the DS's format: BTAF offsets, BTNF names, GMIF data), and each member is packed by the kind in its first byte (undo, as nro0+0x1237c80). Inside: FRES, FSHA, BNTX, VFXB, SARC.
Usage: cra.py <42> <NRO> [<out folder>] lists the files, or writes them out unpacked (a tool for looking; the shader split takes read() and members())."""
import os, struct, sys, zlib
import lz4.block


class Error(ValueError): pass


def crc_table(poly):
    t = [[0] * 256 for _ in range(8)]
    for i in range(256):
        c = i
        for _ in range(8): c = (c >> 1) ^ (poly if c & 1 else 0)
        t[0][i] = c
    for k in range(1, 8):
        for i in range(256): t[k][i] = (t[k - 1][i] >> 8) ^ t[0][t[k - 1][i] & 0xff]
    return t


CRC32, CRC32C = crc_table(0xedb88320), crc_table(0x82f63b78)


def crc64(tab, c, v):   # the crc32x / crc32cx instruction: no inversion before or after
    x = c ^ v
    return (tab[7][x & 0xff] ^ tab[6][x >> 8 & 0xff] ^ tab[5][x >> 16 & 0xff] ^ tab[4][x >> 24 & 0xff] ^ tab[3][x >> 32 & 0xff] ^ tab[2][x >> 40 & 0xff] ^ tab[1][x >> 48 & 0xff] ^ tab[0][x >> 56])


def stem(path):   # the key's input: the first up to 4 characters of the last path element, up to its first '.' or '_', as a big-endian number
    e = path.rsplit('/', 1)[-1]
    n = min((i for i in (e.find('.'), e.find('_'), 4) if i >= 0), default=4)
    return int.from_bytes(e[:n].encode('latin1'), 'big')


def key(path):
    k = crc64(CRC32, stem(path), int.from_bytes(b'UseUrSk1', 'little'))
    a = crc64(CRC32, crc64(CRC32, 0xffffffff, int.from_bytes(b'Nintendo', 'little')), int.from_bytes(b'/CAMELOT', 'little'))
    return (a ^ crc64(CRC32, k, int.from_bytes(b'lls4G00d', 'little')) ^ 0x02052636) & 0xffffffff


VARIANT_MASK = 0x11111111, 0x22222223, 0x33333333, 0x44444445, 0x55555555, 0x66666667, 0x77777777, 0x88888889
M64 = (1 << 64) - 1


def unscramble(path, raw):
    """The blocks of a file, unscrambled, and the sum that gives its checksum. A variant takes a block's low (bit 1 clear) or high word, by CRC-32 (bit 2 clear) or CRC-32C, and writes its two results low
    word first (bit 0 clear) or high word first"""
    w = key(path); v = w & 7
    tab = CRC32C if v & 4 else CRC32
    t0, t1, t2, t3, t4, t5, t6, t7 = tab
    mul = (w & 0xfffffffe ^ VARIANT_MASK[v]) << 32 | 1
    n = len(raw) // 16
    q = struct.unpack(f'<{2 * n}Q', raw[:n * 16])
    out = [0] * (2 * n); total = 0
    src, oth, a, b = (1, 0, 0, 1) if v & 2 else (0, 1, 0, 1)
    if v & 1: a, b = 1, 0
    for i in range(0, 2 * n, 2):
        s = q[i + src]
        x = s ^ w
        m = (t7[x & 0xff] ^ t6[x >> 8 & 0xff] ^ t5[x >> 16 & 0xff] ^ t4[x >> 24 & 0xff] ^ t3[x >> 32 & 0xff] ^ t2[x >> 40 & 0xff] ^ t1[x >> 48 & 0xff] ^ t0[x >> 56]) * mul & M64
        e = q[i + oth] ^ (m >> 3)
        out[i + a] = ~(m ^ (s << 32)) & M64
        out[i + b] = e
        total += e
    return struct.pack(f'<{2 * n}Q', *out), total & M64


def text_end(nro):
    if nro[0x10:0x14] != b'NRO0': raise Error('not an NRO')
    return struct.unpack_from('<I', nro, 0x24)[0] & ~3


def adrp_add(w, i, reg):   # the address adrp + add build in reg at instructions i, i + 1, or None
    a, b = w[i], w[i + 1]
    if a & 0x9f00001f != 0x90000000 | reg or b & 0xffc003ff != 0x91000000 | reg << 5 | reg: return None
    hi = (a >> 5 & 0x7ffff) << 2 | a >> 29 & 3
    if hi & 1 << 20: hi -= 1 << 21
    return (i * 4 & ~0xfff) + (hi << 12) + (b >> 10 & 0xfff)


def index_of(nro):
    """Where the file table, the checksums and the suffixes are in this build, from the code that reads them:
    adrp + add xA, table; mov w, #0x12; ldrsh [xA, wI, sxtw #1]; madd; mov w, #(2 × count), the one place that indexes it with records of 18 bytes
    adrp + add x, checksums; mov x8, #'UseU…', in the read's completion
    adrp + add x, suffixes; ldr x, [x, x, lsl #3], right after the ldrsb of a record's +3"""
    end = text_end(nro)
    w = struct.unpack_from(f'<{end // 4}I', nro)
    table = sums = names = None
    count = 0
    for i in range(3, len(w) - 3):
        c = w[i]
        if table is None and c & 0xffe0fc00 == 0x78e0d800 and w[i - 1] & 0xffffffe0 == 0x52800240 and w[i + 1] & 0xffe08000 == 0x1b000000 and w[i + 2] & 0xffe00000 == 0x52800000:
            if (a := adrp_add(w, i - 3, c >> 5 & 31)) is not None:
                table, count = a, (w[i + 2] >> 5 & 0xffff) // 2
        elif sums is None and c & 0xffffffe0 == 0xd28e6aa0 and w[i + 1] & 0xffffffe0 == 0xf2aaaca0:   # mov xN, #0x7355; movk xN, #0x5565, lsl #16: 'UseU'
            for k in range(i - 3, i):
                for reg in range(31):
                    if (a := adrp_add(w, k, reg)) is not None: sums = a
    if table is None or sums is None: raise Error('this NRO has no CRA file table (or another layout of it)')
    for i in range(len(w) - 4):   # ldrsb w, [x, #3]; tbnz; and; adrp; add
        if w[i] & 0xfffffc00 == 0x39c00c00 and w[i + 1] & 0xff000000 == 0x37000000 and (w[i + 2] & 0xfffffc00) == 0x92401c00:
            for reg in range(31):
                if (a := adrp_add(w, i + 3, reg)) is not None and w[i + 5] & 0xffe0fc00 == 0xf8607800:
                    names = a
    return table, count, sums, names


class Index:
    def __init__(self, nro):
        self.d = nro
        self.table, self.count, self.sums, self.names = index_of(nro)

    def rec(self, i):
        return self.table + struct.unpack_from('<h', self.d, self.table + i * 2)[0] + i * 18 + self.count * 2

    def name(self, i):
        d = self.d; r = self.rec(i); need = d[r + 2]; out = bytearray(need)
        if need:
            k = i - struct.unpack_from('<H', d, r)[0]
            while True:
                s = self.rec(k); have = min(need, d[s + 2])
                for j in range(need - have): out[have + j] = (k - d[s + 0xd + j]) & 0x7f
                need = have
                if not need: break
                k -= struct.unpack_from('<H', d, s)[0]
        o = r + 0xd
        while d[o]:
            out.append((i - d[o]) & 0x7f); o += 1
        sfx = d[r + 3]
        if sfx < 0x80 and self.names is not None:
            o = struct.unpack_from('<Q', d, self.names + sfx * 8)[0]
            while d[o]:
                out.append((sfx - d[o]) & 0x7f); o += 1
        parts = []
        for c in out.decode('latin1').split('/'):
            parts.append(c.replace('$', parts[-1]) if parts else c)
        return '/'.join(parts)

    def files(self):   # (index, path, size, offset, checksum), the empty first record left out
        out = []
        for i in range(self.count):
            r = self.rec(i)
            size = struct.unpack_from('<I', self.d, r + 4)[0] ^ 0xff00ff00
            if not size: continue
            off = (struct.unpack_from('<Q', self.d, r + 8)[0] & 0xffffffffff) ^ 0xff00ff00ff
            out.append((i, self.name(i), size, off, struct.unpack_from('<I', self.d, self.sums + i * 4)[0]))
        return out


def read(f, entry):
    """A file's bytes from the open archive, unscrambled and checked (the checksum covers whole blocks; the last block's tail past the size is padding)"""
    i, path, size, off, want = entry
    f.seek(off)
    plain, total = unscramble(path, f.read((size + 15) & ~15))
    if ((total >> 32) ^ total) & 0xffffffff != want: raise Error(f'{path}: the checksum differs')
    return plain[:size]


def head(f, entry):   # the first block of a file, unscrambled: its magic, to leave out what holds no shaders without unscrambling the rest
    i, path, size, off, want = entry
    f.seek(off)
    return unscramble(path, f.read(16))[0]


def narc_members(d, where='NARC'):
    if d[:4] != b'NARC': raise Error(f'{where}: not a NARC')
    o, n = struct.unpack_from('<HH', d, 0xc); secs = {}
    for _ in range(n):
        if o + 8 > len(d): raise Error(f'{where}: a section past the end')
        s = struct.unpack_from('<I', d, o + 4)[0]; secs[d[o:o + 4]] = o; o += s
    if b'BTAF' not in secs or b'GMIF' not in secs: raise Error(f'{where}: no BTAF or GMIF')
    fat = secs[b'BTAF']; data = secs[b'GMIF'] + 8
    out = []
    for i in range(struct.unpack_from('<I', d, fat + 8)[0]):
        a, b = struct.unpack_from('<II', d, fat + 12 + 8 * i)
        if not data + a <= data + b <= len(d): raise Error(f'{where}: member {i} past the end')
        out.append(d[data + a:data + b])
    return out


def lz_undo(d, at):   # kinds 0x50 and 0x60 (nro0+0x1237f04): flag bytes, each negated and read from its high bit (1 a match, 0 a literal); a flag byte 0 is 8 literals; a distance 0 ends
    out = bytearray(); n = len(d)
    while at < n:
        f = d[at]; at += 1
        if not f:
            out += d[at:at + 8]; at += 8; continue
        f = -f & 0xff
        for bit in range(7, -1, -1):
            if not f >> bit & 1:
                out.append(d[at]); at += 1; continue
            lo, hi = d[at], d[at + 1]
            dist = lo >> 4 | hi << 4
            if not dist: return bytes(out)
            ln = lo & 0xf
            if ln > 1: at += 2
            elif ln == 1: ln = d[at + 2] + 0x11 + (d[at + 3] << 8) + 0xff; at += 4
            else: ln = d[at + 2] + 0x10; at += 3
            s = len(out) - dist
            if s < 0: raise Error('an LZ distance before the start')
            for k in range(ln): out.append(out[s + k])
    return bytes(out)


def zstd_mod():
    try: from compression import zstd
    except ImportError: from backports import zstd   # pyright: ignore[reportMissingImports]  # an optional package, only Python before 3.14 needs it
    return zstd


def undo(d):
    """A member, unpacked by the kind in its first byte, as the game does (nro0+0x1237c80). Anything else is the member itself"""
    if not d: return d
    k = d[0]
    if k == 0x4d and d[:4] == b'MMMM':   # chunks of any kind: u32 total, u32 count, u32 length of each, 16-aligned
        total, n = struct.unpack_from('<II', d, 4)
        at = (12 + 4 * n + 15) & ~15
        out = []
        for i in range(n):
            s = struct.unpack_from('<I', d, 12 + 4 * i)[0]
            out.append(undo(d[at:at + s])); at += (s + 15) & ~15
        return b''.join(out)
    if k == 0x94 and len(d) >= 8: return lz4.block.decompress(d[8:], uncompressed_size=struct.unpack_from('<I', d, 4)[0])
    if k == 0x5a and d[:4] == b'Z\0\0\0': return zstd_mod().decompress(d[8:])
    if k == 0x50 and len(d) >= 8: return lz_undo(d, 8)
    if k == 0x60 and len(d) >= 4: return lz_undo(d, 4)
    if k in (0x70, 0x80) and len(d) >= 0x10: return d[0x10:]
    if k in (0, 0x10, 0x20, 0x30) and len(d) >= 0x10:
        try: return zlib.decompress(d[0x10:], -15 if k == 0x30 else 47)
        except zlib.error: return d   # a member that only starts with such a byte
    return d


def main():
    if len(sys.argv) < 3: sys.exit(__doc__)
    idx = Index(open(sys.argv[2], 'rb').read())
    with open(sys.argv[1], 'rb') as f:
        for e in idx.files():
            print(f'{e[0]}\t{e[1]}\t{e[2]}\t{e[3]:#x}')
            if len(sys.argv) > 3:
                p = os.path.join(sys.argv[3], e[1]); os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, 'wb') as o: o.write(read(f, e))


if __name__ == '__main__': main()
