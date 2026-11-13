#!/usr/bin/env python3
# Rockstar RPF6 archives (RAGE, the format of Red Dead Redemption on PS3 / Xbox 360). Double Eleven's Switch port of Red Dead Redemption (01007820196A6000, 1.0.0) keeps all of its RomFS
# but two Bink movies in 134 of them (11 GB; shaders.rpf, fragments.rpf, audio_switch.rpf, redemption/territory_swall/*.rpf, …). shader_split.py reads them through this module (rpf_jobs).
# Layout (big-endian, as on the consoles):
#   header  16 bytes: 'RPF6', u32 entry count, u32 name table offset / 8, u32 encryption tag (0xfffffffd in every Switch archive: the entry table and the name table are encrypted)
#   entries entry count × 20 bytes from +0x10, AES-256 ECB decrypted 16 times over (as on the consoles), padded to 16 bytes:
#           a directory: u32 name hash, u32 0 (bit 31 set on some), u32 0x80000000 | first child's entry, u32 child count, u32 0
#           a file:      u32 name hash, u32 stored length (low 31 bits), u32 data offset / 8, u32 flags, u32 0; flags bit 31 clear: bit 30 compressed | length (low 30 bits)
#           a resource:  flags bit 31 set (act.rpf's .wat, fragments.rpf's .wft, mapres.rpf's .wtx, …): the data offset word's low byte is the resource type (0x1e in act.rpf), so the offset is
#                        (word & 0x7fffff00) × 8; the flags are RAGE's resource flags (bit 30 compressed, the rest page sizes), not a length. The data is an RSC5 header {'RSC\x05', u32 type
#                        (little-endian), u32 the entry's flags (little-endian)}, then one zstd frame; the stored length counts the header. Reading the offset as a plain file's gave
#                        145 groups of unreadable files in the first build. A resource whose flags are only bit 31 (gringores.rpf's .wgd, vfx.rpf's .wtd, the .wsc scripts) has an RSC85
#                        header instead: {'RSC\x85', u32 type, u32 the entry's flags, u32 the entry's fifth word (its extended flags)}, 16 bytes
#   data    a zstd frame or a zlib stream (unpack), or raw deflate (audio_switch.rpf's config files, as on the consoles); the scripts (.wsc, RSC85 type 2) are in addition AES encrypted like the tables (whole blocks; the tail of under 16 bytes is clear)
#           entry 0 is the root directory (hash 0); a directory's children are consecutive entries
#   names   at the name table offset to the end of the file, encrypted the same way: a binary table, then the names as NUL-terminated strings; a name hash is Jenkins one-at-a-time
#           (joaat) of the lower-case name, so names are found by hashing every string there (shaders.rpf: 134 of 134 hashes)
# A compressed file is one zstd frame (the consoles used zlib; shaders.rpf: 131 files, every frame's content size equals the entry's length).
# The key is in main's data segment (1.0.0: main+0x3757f88), found as the 32-byte window that decrypts the first entry to the root directory (find_key); it is never written to disk.
# Update 1.0.4 (v262144) has the same key, but not as 32 bytes in a row: its key setup (main+0x145d8c) loads the eight key words, each a big-endian u32 zero-extended to 64 bits,
# from four 16-byte constants in the read-only segment (key_from_setup). Update 1.0.7 (v458752) has 1.0.4's main. find_key tries those candidates first: on 1.0.4 / 1.0.7 the window search
# finds nothing after millions of tries.
# Usage: rpf6.py <main NSO> <file.rpf> [--extract <dir>] lists (or extracts) the files.
import os, struct, sys, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import open
from uepak import aes_decrypt

MAGIC, ENCRYPTED, RSC5, RSC85, ZSTD = b'RPF6', 0xfffffffd, b'RSC\x05', b'RSC\x85', b'\x28\xb5\x2f\xfd'
ROUNDS = 16

def joaat(s):
    """Jenkins one-at-a-time over the lower-case name: RAGE's name hash"""
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xffffffff; h = (h + (h << 10)) & 0xffffffff; h ^= h >> 6
    h = (h + (h << 3)) & 0xffffffff; h ^= h >> 11
    return (h + (h << 15)) & 0xffffffff

def decrypt(key, data):
    """RPF6's table encryption: AES-256 ECB applied 16 times (whole blocks only)"""
    data = data[:len(data) // 16 * 16]
    for _ in range(ROUNDS): data = aes_decrypt(key, data)
    return data

def is_root(d):
    """The root directory entry: hash 0, 0, first child 1 (the entries after it), at least one child"""
    return len(d) >= 16 and struct.unpack_from('>3I', d) == (0, 0, 0x80000001) and struct.unpack_from('>I', d, 12)[0] > 0

def header(path):
    """→ (entry count, name table offset, encrypted), or None when path is no RPF6"""
    with open(path, 'rb') as f: h = f.read(16)
    if len(h) < 16 or h[:4] != MAGIC: return None
    n, names, tag = struct.unpack_from('>3I', h, 4)
    return n, names * 8, tag == ENCRYPTED

def find_key(nso_path, rpf):
    """The AES key that decrypts rpf's first entry to the root directory: first the keys the key setup in main's code builds (key_from_setup: 1.0.4 / 1.0.7, a few candidates),
    then every 4-aligned 32-byte window (at least 24 distinct bytes) of main's read-only and data segments (1.0.0: one window, in the data segment; millions of tries)"""
    import nso
    with open(nso_path, 'rb') as f: m = nso.load(f.read(), nso_path)
    img = bytes(m.image)
    with open(rpf, 'rb') as f: f.seek(16); blk = f.read(16)
    for k in key_from_setup(img, m.segments[0]):
        if is_root(decrypt(k, blk)): return k
    for mo, sz in m.segments[1:]:   # read-only, data
        for a in range(mo, mo + sz - 32, 4):
            w = img[a:a + 32]
            if len(set(w)) >= 24 and is_root(decrypt(w, blk)): return w
    return None

# The key setup's first instructions (1.0.0: main+0x21ec0c, 1.0.4: main+0x145d8c): cmp w1, #0; mov w8, #-3; csel w8, w8, w1, eq; cmn w8, #3; str w8, [x0]
SETUP = struct.pack('<5I', 0x7100003f, 0x12800048, 0x1a810108, 0x31000d1f, 0xb9000008)

def key_from_setup(img, text):
    """Candidate keys from each key setup in main's code: after its b.ne, the function stores 64 bytes of 16-byte constants (adrp, then ldr q) at x0+8 (str q / stp q, through
    registers copied from x0 and moved by add); those are the eight key words as big-endian u32s zero-extended to 64 bits (1.0.4). A setup that stores something else gives no candidate.
    The caller keeps only a candidate that opens the archive"""
    ts, tl = text
    at = img.find(SETUP, ts, ts + tl)
    while at >= 0:
        page, ctx, q, mem = {}, {0: 0}, {}, {}
        for a in range(at + len(SETUP) + 4, min(at + 0x80, ts + tl), 4):   # past the b.ne
            w = struct.unpack_from('<I', img, a)[0]
            rd, rn = w & 31, (w >> 5) & 31
            if w & 0x9f000000 == 0x90000000:   # adrp
                imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
                page[rd] = (a & ~0xfff) + ((imm - (1 << 21) if imm & (1 << 20) else imm) << 12); ctx.pop(rd, None)
            elif w & 0xffe0ffe0 == 0xaa0003e0 and (w >> 16) & 31 in ctx: ctx[rd] = ctx[(w >> 16) & 31]   # mov xd, xm
            elif w & 0xffc00000 == 0x91000000 and rn in ctx: ctx[rd] = ctx[rn] + ((w >> 10) & 0xfff)   # add xd, xn, #imm
            elif w & 0xffc00000 == 0x3dc00000 and rn in page:   # ldr qt, [xn, #imm]
                o = page[rn] + ((w >> 10) & 0xfff) * 16
                q[rd] = img[o:o + 16]
            elif w & 0xffe00c00 == 0x3c800c00 and rn in ctx and rd in q:   # str qt, [xn, #imm]!
                imm = (w >> 12) & 0x1ff; ctx[rn] += imm - 0x200 if imm & 0x100 else imm; mem[ctx[rn]] = q[rd]
            elif w & 0xffc00000 == 0x3d800000 and rn in ctx and rd in q: mem[ctx[rn] + ((w >> 10) & 0xfff) * 16] = q[rd]   # str qt, [xn, #imm]
            elif w & 0xffc00000 == 0xad000000 and rn in ctx and rd in q and (w >> 10) & 31 in q:   # stp qt, qt2, [xn, #imm]
                imm = (w >> 15) & 0x7f; o = ctx[rn] + (imm - 0x80 if imm & 0x40 else imm) * 16
                mem[o], mem[o + 16] = q[rd], q[(w >> 10) & 31]
            elif w & 0xfc000000 in (0x14000000, 0x94000000) or w & 0xff000010 == 0x54000000: break   # b, bl, b.cond: the stores are done
        if all(o in mem for o in range(8, 0x48, 16)):
            words = struct.unpack('<8Q', b''.join(mem[o] for o in range(8, 0x48, 16)))
            if all(v >> 32 == 0 for v in words): yield struct.pack('>8I', *words)
        at = img.find(SETUP, at + 4, ts + tl)

class Rpf:
    def __init__(self, path, key):
        h = header(path)
        if h is None: raise ValueError(f'{path}: no RPF6 archive')
        self.path, self.key, (n, names_at, enc) = path, key, h
        with open(path, 'rb') as f:
            f.seek(16); t = f.read((n * 20 + 15) // 16 * 16)
            f.seek(names_at); nt = f.read()
        if enc: t, nt = decrypt(key, t), decrypt(key, nt)
        if not is_root(t): raise ValueError(f'{path}: the first entry is no root directory (wrong key?)')
        self.raw = [struct.unpack_from('>5I', t, 20 * i) for i in range(n)]
        names = {joaat(s.decode()): s.decode() for s in re.findall(rb'[\x20-\x7e]{1,255}(?=\0)', nt)}
        self.files = []   # (path, offset, stored length, length (None for a resource), compressed, resource flags (None for a plain file))
        def walk(i, prefix):
            h, a, b, c, _ = self.raw[i]
            name = names.get(h, f'#{h:08x}')
            if b & 0x80000000:
                first, count = b & 0x7fffffff, c
                if first + count > n: raise ValueError(f'{path}: directory entry {i} runs past the table')
                for j in range(first, first + count): walk(j, prefix + name + '/' if i else '')
            elif c & 0x80000000: self.files.append((prefix + name, (b & 0x7fffff00) * 8, a & 0x7fffffff, None, bool(c & 0x40000000), c))
            else: self.files.append((prefix + name, (b & 0x7fffffff) * 8, a & 0x7fffffff, c & 0x3fffffff, bool(c & 0x40000000), None))
        walk(0, '')

    def read(self, f, entry, zstd):
        name, off, stored, size, comp, rflags = entry
        f.seek(off); d = f.read(stored)
        if len(d) != stored: raise ValueError(f'{self.path}/{name}: cut off ({len(d)} of {stored} bytes)')
        if rflags is not None:   # a resource: RSC5 or RSC85 header, then its data
            if d[:4] not in (RSC5, RSC85): raise ValueError(f'{self.path}/{name}: a resource entry, but no RSC5 / RSC85 header at {off:#x}')
            if struct.unpack_from('<I', d, 8)[0] != rflags: raise ValueError(f'{self.path}/{name}: the resource header\'s flags differ from the entry\'s')
            d = d[16 if d[:4] == RSC85 else 12:]
        if not comp and rflags is None: return d
        u = unpack(d, zstd)
        if u is None and comp and size is not None:   # raw deflate, as on the consoles (audio_switch.rpf's .dat / .dat15 / .dat46 config files): kept only at the entry's length
            import zlib
            try: u = zlib.decompressobj(-15).decompress(d)
            except zlib.error: u = None
            if u is not None and len(u) != size: u = None
        if u is None and self.key:   # a script (.wsc): encrypted like the tables, then compressed
            u = unpack(decrypt(self.key, d) + d[len(d) // 16 * 16:], zstd)
        if u is None:
            if not comp: return d   # a resource stored as is
            raise ValueError(f'{self.path}/{name}: compressed, but neither a zstd frame nor a zlib stream, also after decryption')
        if size is not None and len(u) != size: raise ValueError(f'{self.path}/{name}: {len(u)} bytes decoded, the entry says {size}')
        return u

def unpack(d, zstd):
    """The decoded data when d is a zstd frame or a zlib stream, else None. Both occur: most files are zstd, some resources zlib (cero/fragments.rpf's .wedt, the RSC\\x85 ones)"""
    if d[:4] == ZSTD: return zstd.decompress(d)
    if len(d) >= 2 and d[0] & 0xf == 8 and d[0] >> 4 <= 7 and (d[0] << 8 | d[1]) % 31 == 0:
        import zlib
        try: return zlib.decompressobj().decompress(d)
        except zlib.error: return None
    return None

if __name__ == '__main__':
    if len(sys.argv) < 3: sys.exit(__doc__ or 'usage: rpf6.py <main NSO> <file.rpf> [--extract <dir>]')
    key = find_key(sys.argv[1], sys.argv[2])
    if key is None: sys.exit(f'[RED] no window of main opens {sys.argv[2]}')
    r = Rpf(sys.argv[2], key)
    out = sys.argv[sys.argv.index('--extract') + 1] if '--extract' in sys.argv else None
    if out:
        from shader_split import zstd_mod
        z = zstd_mod()
    with open(r.path, 'rb') as f:
        for e in r.files:
            print(f'{"rsc" if e[3] is None else e[3]:>10} {"z" if e[4] else "-"} {e[0]}')
            if out:
                p = os.path.join(out, e[0]); os.makedirs(os.path.dirname(p), exist_ok=True)
                with open(p, 'wb') as g: g.write(r.read(f, e, z))
