# NSO → 内存映像（与 patch_nso.py 同一解法）；验证工具共用。
# NSO → memory image (same decoding as patch_nso.py); shared by the verification tools.
import struct
import lz4.block
from relr import decode_relr


def direct_target(word, address):
    if word & 0x7c000000 == 0x14000000:
        return address + 4 * (((word & 0x3ffffff) ^ 0x2000000) - 0x2000000)
    if word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000:
        return address + 4 * ((((word >> 5) & 0x7ffff) ^ 0x40000) - 0x40000)
    if word & 0x7e000000 == 0x36000000:
        return address + 4 * ((((word >> 5) & 0x3fff) ^ 0x2000) - 0x2000)
    return None


def exclusive_groups(image, windows):
    """Independently connect original atomic fragments by their direct edges."""
    parents = {lo: lo for lo, _ in windows}
    def find(node):
        while parents[node] != node:
            parents[node] = parents[parents[node]]; node = parents[node]
        return node
    for lo, hi in windows:
        for address in range(lo, hi, 4):
            target = direct_target(struct.unpack_from('<I', image, address)[0], address)
            if target is None: continue
            other = next((a for a, b in windows if a <= target < b), None)
            if other is not None: parents[find(other)] = find(lo)
    groups = {}
    for lo, hi in windows: groups.setdefault(find(lo), []).append((lo, hi))
    return {lo: group for group in groups.values() for lo, _ in group}

def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'NSO0', path
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x3c)[0]
    csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2] + bss)
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
        img[mo:mo + sz] = raw
    return bytes(img)

def header(out_dir, mod):
    h = {}
    for line in open(f'{out_dir}/{mod}.fixups.tsv'):
        if not line.startswith('#'): break
        k, v = line[1:].split(); h[k] = int(v, 0)      # 0x 开头为偏移，十进制为槽号 | A 0x prefix means an offset, decimal means a slot number
    return h

def mod0(img):
    m0 = struct.unpack_from('<I', img, 4)[0]
    assert img[m0:m0 + 4] == b'MOD0'
    return m0

def func_starts(img, text_end):
    """eh_frame_hdr 里的函数起点（与 patch_nso.py 同法）。 | Function start addresses from eh_frame_hdr (same method as patch_nso.py)."""
    m0 = mod0(img)
    ehs = m0 + struct.unpack_from('<i', img, m0 + 16)[0]
    def rd(o, enc):
        fmt, app = enc & 0xF, enc & 0x70
        v, sz = {0x3: (struct.unpack_from('<I', img, o)[0], 4), 0xB: (struct.unpack_from('<i', img, o)[0], 4),
                 0x4: (struct.unpack_from('<Q', img, o)[0], 8)}[fmt]
        if app == 0x10: v += o
        if app == 0x30: v += ehs
        return v, sz
    _, penc, cenc, tenc = img[ehs:ehs + 4]
    o = ehs + 4
    _, s = rd(o, penc); o += s
    nfde, s = rd(o, cenc); o += s
    starts = set()
    for _ in range(nfde):
        a, s = rd(o, tenc); o += s
        _, s = rd(o, tenc); o += s
        starts.add(a)
    return sorted(x for x in starts if x < text_end)

def relocs(img):
    """[(偏移, 类型, 符号名或 None, 符号值或 None, addend)]：RELA + JMPREL 全部条目。符号值 None = 未定义（导入）。 | [(offset, type, symbol name or None, symbol value or None, addend)]: all RELA + JMPREL entries. Symbol value None = undefined (import)."""
    m0 = mod0(img)
    dyn = m0 + struct.unpack_from('<i', img, m0 + 4)[0]
    d = {}
    o = dyn
    while True:
        tag, val = struct.unpack_from('<qQ', img, o); o += 16
        if tag == 0: break
        d[tag] = val
    def cstr(p):
        e = img.index(b'\0', p); return img[p:e].decode('latin1')
    out = [(where, 1027, None, None, addend) for where, addend in decode_relr(img, d, 0, len(img))]
    for tab, sz in ((d.get(7), d.get(8, 0)), (d.get(23), d.get(2, 0))):
        if not tab: continue
        for i in range(sz // 24):
            off, info, add = struct.unpack_from('<QQq', img, tab + 24 * i)
            typ, si = info & 0xFFFFFFFF, info >> 32
            name = val = None
            if si:
                nm, _, _, shn, sv, _ = struct.unpack_from('<IBBHQQ', img, d[6] + 24 * si)
                name = cstr(d[5] + nm); val = sv if shn else None
            out.append((off, typ, name, val, add))
    return out
