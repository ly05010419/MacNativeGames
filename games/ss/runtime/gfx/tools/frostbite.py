#!/usr/bin/env python3
"""Frostbite's game data on the Switch (EA SPORTS FC 27, Frostbite 2025.1.2): the superbundle tables (.toc), the bundle manifests and the compressed data in the cas files, and the
NVN shaders of its CompiledBytecode resources. Used by shader_split.py (frostbite_jobs, frostbite_shaders) and dm/games/profile_guess.py (is_frostbite).

The RomFS holds Data/ (the base data) and Patch/ (the update's), each with NX/<superbundle>.toc files and NX/superbundlelayout/<install chunk>/cas_NN.cas files, and Data/layout.toc,
whose installManifest names the install chunks. Every .toc starts with the magic 00 D1 CE 01, a signature and padding; its body is at +0x22c. Data/initfs_NX is encrypted (the game decrypts
it); nothing here needs it.
· layout.toc's body is a DbObject (dbobject): installManifest.installChunks lists {name 'superbundlelayout/<name>', the folder under NX/, persistentIndex}, which the cas ids name
  (installBundle is the same path under NX/, empty for two install chunks with no bundles of their own).
· A superbundle .toc's body (big-endian): u32 offsets and counts, relative to the body: +4 bundle table, +8 bundle count, +0x14 chunk count, +0x18 chunk table. A bundle is
  {u32 name, u32 0x40000000 | data size, u64 data offset}; its data is {u32 0, 0, flag offset, entry count, entry offset, …}, then the entries, then one flag byte per entry: an entry is
  {u32 offset, u32 stored size} in a cas file, preceded by an 8-byte cas id when its flag has bit 7 set (the first one always). A cas id is {u8 0, u8 1 = Patch / 0 = Data,
  u32 the install chunk's persistentIndex, u16 the cas file's number}. Entry 0 is the bundle's manifest, the others its items in the manifest's order (ebx, res, chunks).
  A chunk is {cas id, u32 offset, u32 stored size}, 16 bytes.
· A manifest is stored as it is: u32 big-endian length, then (little-endian) the magic 0x9D798ED6, total count, ebx count, res count, chunk count, string offset (from the magic),
  meta offset, meta size, a SHA-1 per item, then the ebx {u32 name, u32 size}, the res {u32 name, u32 size}, the res types (u32 each, name_hash of the type's name), 16 bytes of meta and
  a u64 id per res, the chunks {16-byte GUID, u32 logical offset, u32 logical size}, the names.
· An item's stored data is a run of blocks: {u32 big-endian size (low 24 bits), u8 codec, u8 0x70 | bits 16..19 of the stored size, u16 big-endian stored size bits 0..15} and the
  stored bytes. Codec 0 is stored as it is, 0x19 Oodle (Leviathan, 8C 0C), which only the game's own decoder reads (shader_split.py oodle). Blocks hold at most 256 KB.
· A CompiledBytecode resource (shaders/bytecode/<guid>) is a 0x20-byte header (u8 5, u8 stage: 0 vertex, 1 pixel, 5 compute; …) and a VNVN record, fb::NvnCustomShaderData
  (Engine.Render.Core2.PlatformNvn.nro, NvnCustomShaderData::read): 'VNVN', u32 size, u16 version (9), …, a byte from version 3 on, four lists of register mappings {u16 count, 4 bytes
  each}, one list {u16 count, 8 bytes each}, then {u32 code offset, u32 code length, u32 control offset, u32 control length, u64 GPU code bits}, offsets from the record. The code area and
  control section are NVN's own (compiled offline); 10,401 of them in FC 27 1.8d.310c, 7,084 graphics and 3,317 compute."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open, mmap

TOC_MAGIC, TOC_BODY = bytes.fromhex('00d1ce01'), 0x22c
BUNDLE_MAGIC = 0x9D798ED6


def name_hash(name):
    """Frostbite's hash of a type name (res types): djb2 with xor over the lower-case name; name_hash('CompiledBytecode') = 0x0e2b02f7, 'Texture' 0x6bde20ba, 'MeshSet' 0x49b156d4"""
    h = 5381
    for c in name.lower().encode(): h = (h * 33 ^ c) & 0xffffffff
    return h


COMPILED_BYTECODE = name_hash('CompiledBytecode')


def is_frostbite(romfs):
    """The RomFS is Frostbite's: Data/layout.toc with the toc magic"""
    p = os.path.join(romfs, 'Data', 'layout.toc')
    if not os.path.exists(p): return False
    with open(p, 'rb') as f: return f.read(4) == TOC_MAGIC


def _leb(d, p):
    v = s = 0
    while True:
        if p >= len(d): raise ValueError('DbObject: a length runs past the end')
        b = d[p]; p += 1; v |= (b & 0x7f) << s; s += 7
        if not b & 0x80: return v, p


def dbobject(d, p):
    """One DbObject value at p → ((name or None, value), next offset). A type byte (bit 7 clear: a NUL-terminated name follows; low 5 bits the kind: 1 list, 2 object, 6 bool, 7 string,
    8 int32, 9 int64, 0xb float, 0xc double, 0xf GUID, 0x10 SHA-1, 0x13 blob), lists and objects with a LEB128 length and ending in a 0 byte"""
    t = d[p]; p += 1; name = None
    if not t & 0x80:
        e = d.index(b'\0', p); name = d[p:e].decode(errors='replace'); p = e + 1
    k = t & 0x1f
    if k in (1, 2):
        n, p = _leb(d, p); end = p + n; items = []
        while p < end and d[p]: it, p = dbobject(d, p); items.append(it)
        return (name, dict(items) if k == 2 else [v for _, v in items]), end
    if k == 7: n, p = _leb(d, p); return (name, d[p:p + n - 1].decode(errors='replace')), p + n
    if k == 19: n, p = _leb(d, p); return (name, bytes(d[p:p + n])), p + n
    fixed = {6: ('<?', 1), 8: ('<i', 4), 9: ('<q', 8), 0xb: ('<f', 4), 0xc: ('<d', 8)}
    if k in fixed: return (name, struct.unpack_from(fixed[k][0], d, p)[0]), p + fixed[k][1]
    if k in (0xf, 0x10): n = 16 if k == 0xf else 20; return (name, bytes(d[p:p + n])), p + n
    raise ValueError(f'DbObject: type {t:#x} at {p - 1:#x} is not known')


def _read(p):
    with open(p, 'rb') as f: return f.read()


def install_chunks(romfs):
    """{persistentIndex (u32): the install chunk's folder under NX/, 'superbundlelayout/<name>'} from Data/layout.toc"""
    d = _read(os.path.join(romfs, 'Data', 'layout.toc'))
    (_, lay), _ = dbobject(d, TOC_BODY)
    if not isinstance(lay, dict) or 'installManifest' not in lay: raise ValueError('Data/layout.toc: no installManifest')
    return {c['persistentIndex'] & 0xffffffff: c['name'] for c in lay['installManifest']['installChunks']}


def tocs(romfs):
    """The superbundle tables of Data/ and Patch/ (Patch/ holds the update's bundles and chunks, which name Patch/ cas files)"""
    out = []
    for top in ('Data', 'Patch'):
        for dp, _, fs in os.walk(os.path.join(romfs, top, 'NX')):
            out += [os.path.join(dp, f) for f in sorted(fs) if f.endswith('.toc')]
    return out


def toc_bundles(d, what='toc'):
    """[[(cas id, offset, stored size), …] per bundle] of a superbundle .toc (entry 0 the manifest); [] for a toc without a body"""
    if d[:4] != TOC_MAGIC: raise ValueError(f'{what}: not a Frostbite toc (magic {bytes(d[:4]).hex()})')
    if len(d) < TOC_BODY + 0x20: return []
    B = TOC_BODY; bo, nb = struct.unpack_from('>II', d, B + 4)
    out = []
    for i in range(nb):
        _, sz, off = struct.unpack_from('>IIQ', d, B + bo + 16 * i)
        s = B + off; end = s + (sz & 0xffffff)
        if end > len(d): raise ValueError(f'{what}: bundle {i}\'s data runs past the end')
        fo, n, p = struct.unpack_from('>III', d, s + 8)
        p += s; fo += s; cas, ents = None, []
        if fo + n > end: raise ValueError(f'{what}: bundle {i}\'s flags run past its data')
        for k in range(n):
            if d[fo + k] & 0x80 or cas is None: cas = bytes(d[p:p + 8]); p += 8
            ents.append((cas,) + struct.unpack_from('>II', d, p)); p += 8
        if p != fo: raise ValueError(f'{what}: bundle {i}\'s entries end at {p - s:#x}, its flags start at {fo - s:#x}')
        out.append(ents)
    return out


def cas_path(romfs, cid, chunks):
    """The cas file a cas id names"""
    pi, n = struct.unpack_from('>IH', cid, 2)
    if pi not in chunks: raise ValueError(f'cas id {cid.hex()}: no install chunk has persistentIndex {pi:#x}')
    return os.path.join(romfs, 'Patch' if cid[1] else 'Data', 'NX', chunks[pi], f'cas_{n:02d}.cas')


def manifest(d, off):
    """A bundle manifest at off in a cas file → (manifest length, [(ebx name, size)], [(res name, size, type)], [(chunk GUID, logical offset, logical size)])"""
    hl = struct.unpack_from('>I', d, off)[0]; magic, tot, ne, nr, nc, so = struct.unpack_from('<6I', d, off + 4)
    if magic != BUNDLE_MAGIC: raise ValueError(f'no bundle manifest at {off:#x} (magic {magic:#x})')
    if ne + nr + nc != tot: raise ValueError(f'bundle manifest at {off:#x}: {ne} + {nr} + {nc} items, not the {tot} it counts')
    h = off + 4; p = h + 0x20 + 20 * tot
    ebx = [struct.unpack_from('<II', d, p + 8 * i) for i in range(ne)]; p += 8 * ne
    res = [struct.unpack_from('<II', d, p + 8 * i) for i in range(nr)]; p += 8 * nr
    types = struct.unpack_from(f'<{nr}I', d, p); p += 4 * nr + 24 * nr
    ch = [struct.unpack_from('<16sII', d, p + 24 * i) for i in range(nc)]
    def name(o):
        a = h + so + o; e = d.find(b'\0', a, h + hl)
        if e < 0: raise ValueError(f'bundle manifest at {off:#x}: a name at {a - off:#x} runs past the manifest')
        return bytes(d[a:e]).decode(errors='replace')
    return hl + 4, [(name(a), s) for a, s in ebx], [(name(a), s, t) for (a, s), t in zip(res, types)], [(g, lo, ls) for g, lo, ls in ch]


def resources(romfs, rtype):
    """Every res of type rtype (a name_hash) the bundles of every superbundle hold → {(cas path, offset, stored size): (size, name)}, each stored copy once. Data/ and Patch/ are read
    alike: a shader's key is its content, so a base copy the update replaced only adds a shader the game no longer uses"""
    chunks, maps, out = install_chunks(romfs), {}, {}
    def view(p):
        if p not in maps:
            with open(p, 'rb') as f: maps[p] = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        return maps[p]
    try:
        for t in tocs(romfs):
            for ents in toc_bundles(_read(t), t):
                cid, off, size = ents[0]
                mlen, ebx, res, ch = manifest(view(cas_path(romfs, cid, chunks)), off)
                if mlen != size: raise ValueError(f'{t}: a bundle manifest of {mlen:#x} bytes, the toc says {size:#x}')
                if len(ebx) + len(res) + len(ch) != len(ents) - 1: raise ValueError(f'{t}: a bundle of {len(ebx) + len(res) + len(ch)} items with {len(ents) - 1} entries')
                for (nm, sz, ty), (c, o, s) in zip(res, ents[1 + len(ebx):]):
                    if ty == rtype: out[(cas_path(romfs, c, chunks), o, s)] = (sz, nm)
    finally:
        for m in maps.values(): m.close()
    return out


def read(d, off, stored, size, decode):
    """An item's data: its blocks at off (stored bytes in all) decoded; decode(compressed, size) is the game's Oodle"""
    o, end, out = off, off + stored, bytearray()
    while o < end:
        if o + 8 > end: raise ValueError(f'a block header at {o:#x} runs past the item')
        n, codec, cs = struct.unpack_from('>IBB', d, o)[0] & 0xffffff, d[o + 4], d[o + 5]
        c = (cs & 0xf) << 16 | struct.unpack_from('>H', d, o + 6)[0]
        if cs & 0xf0 != 0x70 or o + 8 + c > end: raise ValueError(f'a damaged block at {o:#x} ({bytes(d[o:o + 8]).hex()})')
        blk = d[o + 8:o + 8 + c]
        if codec == 0: out += blk
        elif codec == 0x19: out += decode(bytes(blk), n)
        else: raise ValueError(f'block at {o:#x}: codec {codec:#x} is not known')
        o += 8 + c
    if len(out) != size: raise ValueError(f'item at {off:#x}: {len(out):#x} bytes decoded, not {size:#x}')
    return bytes(out)


def nvn_shader(b):
    """A CompiledBytecode resource → (code area, control section), or None when it holds no VNVN record"""
    if len(b) < 0x24 or b[0x20:0x24] != b'VNVN': return None
    r = 0x20; v = struct.unpack_from('<H', b, r + 8)[0]; p = r + (0x11 if v >= 3 else 0x10)
    for _ in range(4): p += 2 + 4 * struct.unpack_from('<H', b, p)[0]
    p += 2 + 8 * struct.unpack_from('<H', b, p)[0]
    co, cl, ko, kl = struct.unpack_from('<4I', b, p)
    if r + co + cl > len(b) or r + ko + kl > len(b): raise ValueError(f'VNVN record: code {co:#x}+{cl:#x} or control {ko:#x}+{kl:#x} outside its {len(b) - r:#x} bytes')
    return b[r + co:r + co + cl], b[r + ko:r + ko + kl]
