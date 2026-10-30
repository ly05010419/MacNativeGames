#!/usr/bin/env python3
# Which loaded entry points does a module's code really call? For loaders that fetch functions by name and keep each pointer in a global (glad's GL loader over eglGetProcAddress,
# NVN's C proc loader over nvnDeviceGetProcAddress): the loader is a run of "name → x0 or x1 (adrp + add), call the get-proc function, adrp + ldr of a GOT slot, str x0 through it".
# Every GOT slot stored that way is named after its string; the code outside the loader then reaches a function by loading its slot (adrp + ldr of the same GOT address), so the
# slots loaded elsewhere are the functions the game calls. Digimon Story Cyber Sleuth: Complete Edition: glad asks for 2,956 GL names, the code loads 79 of them; its NVN loader
# fetches 512, the code loads 30 (docs/agents/02-survey.md).
# Usage: loader_census.py <NSO or decompressed image> [--sites name,name...]   (an NSO is decompressed in memory; a .bin is taken as is, text from 0 to the end of the first segment it
# cannot tell, so give an NSO when you can)
import struct, sys, collections

def image(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0': return d, len(d)
    import lz4.block
    flags = struct.unpack_from('<I', d, 0xc)[0]; segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img), segs[0][2]

def adrp(w, pc):
    if (w & 0x9f000000) != 0x90000000: return None
    imm = ((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return w & 31, (pc & ~0xfff) + imm
def addimm(w): return (w & 31, (w >> 5) & 31, (w >> 10) & 0xfff) if (w & 0xffc00000) == 0x91000000 else None
def ldr64(w): return (w & 31, (w >> 5) & 31, ((w >> 10) & 0xfff) * 8) if (w & 0xffc00000) == 0xf9400000 else None
def is_call(w): return (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000   # bl, blr
def is_str_x0(w, base): return (w & 0xffc003ff) == 0xf9000000 | (base << 5) and ((w >> 10) & 0xfff) == 0   # str x0, [base]
def str_x0(w): return ((w >> 5) & 31, ((w >> 10) & 0xfff) * 8) if (w & 0xffc0001f) == 0xf9000000 else None   # str x0, [base, #off] → (base, off)

def got_values(img):
    """{address: the address it holds once relocated} for the image's relative relocations (RELA R_AARCH64_RELATIVE, and RELR, whose addend is in place)"""
    out = {}
    mod0 = struct.unpack_from('<I', img, 4)[0]
    if img[mod0:mod0 + 4] != b'MOD0': return out
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]; tags = {}
    for k in range(4096):
        t, v = struct.unpack_from('<qQ', img, dyn + k * 16)
        if t == 0: break
        tags.setdefault(t, v)
    for k in range(tags.get(8, 0) // 24):   # DT_RELA / DT_RELASZ: RELATIVE, and ABS64 / GLOB_DAT of a symbol the module defines (Balatro's glad pointers are exported)
        off, info, add = struct.unpack_from('<QQq', img, tags[7] + k * 24)
        if info & 0xffffffff == 1027: out[off] = add
        elif info & 0xffffffff in (257, 1025) and 6 in tags:
            shndx, value = struct.unpack_from('<HQ', img, tags[6] + (info >> 32) * 24 + 6)
            if shndx: out[off] = value + add
    where = 0
    for k in range(tags.get(35, 0) // 8):   # DT_RELR / DT_RELRSZ
        e = struct.unpack_from('<Q', img, tags[36] + k * 8)[0]
        if not e & 1: where = e; out[where] = struct.unpack_from('<Q', img, where)[0]; where += 8
        else:
            for b in range(63):
                if e >> (b + 1) & 1: a = where + b * 8; out[a] = struct.unpack_from('<Q', img, a)[0]
            where += 63 * 8
    return out

def known_loads(words, got=None):
    """(index, address) of every 64-bit load whose base register holds a known address: adrp, adds to it, mov and csel of known ones, followed in a straight line (a return forgets
    them all; a call or a branch keeps only x19–x28, which a function's hoisted pointers stay in; any instruction that may write a register forgets that one); a load from a slot in
    got (address → the address it holds) gives its register that address. Digimon reaches a function through its GOT slot (adrp + ldr); LÖVE's glad keeps its pointers in globals
    that the code reaches through GOT entries and keeps in registers (Balatro: adrp + ldr x22 of the GOT entry, ldr x8, [x22], blr x8)"""
    got = got or {}
    val = {}   # register → the addresses it may hold (two after a csel of known ones: LÖVE picks glEnable or glDisable, glEnableVertexAttribArray or glDisableVertexAttribArray so)
    for i, w in enumerate(words):
        r = adrp(w, i * 4)
        if r: val[r[0]] = (r[1],); continue
        a = addimm(w)
        if a:
            if a[1] in val: val[a[0]] = tuple(x + a[2] for x in val[a[1]])
            else: val.pop(a[0], None)
            continue
        l = ldr64(w)
        if l:
            ats = tuple(x + l[2] for x in val.get(l[1], ()))
            for at in ats: yield i, at
            held = tuple(got[at] for at in ats if at in got)   # a GOT slot holds a global's address: the next load off it reads the global
            if held: val[l[0]] = held
            else: val.pop(l[0], None)
            continue
        mov, csel = (w & 0xffe0ffe0) == 0xaa0003e0, (w & 0xffe00c00) == 0x9a800000
        if mov or csel:   # mov xd, xm; csel xd, xn, xm
            src = [(w >> 16) & 31] + ([(w >> 5) & 31] if csel else [])
            if all(k in val for k in src): val[w & 31] = tuple(x for k in src for x in val[k])
            else: val.pop(w & 31, None)
            continue
        if (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000:   # bl, blr: x0–x18 and x30 are the callee's
            for k in [*range(19), 30]: val.pop(k, None)
            continue
        if (w & 0xfc000000) == 0x14000000:   # b: the next block is reached from elsewhere, where x19–x28 still hold what the function hoisted into them (a loop's GL pointers)
            for k in [*range(19), 29, 30]: val.pop(k, None)
            continue
        if w == 0xd65f03c0 or (w & 0xfffffc1f) == 0xd61f0000:   # ret, br: the function ends
            val.clear(); continue
        if (w & 0x0a000000) == 0x08000000:   # loads and stores
            if (w & 0x3a000000) == 0x28000000:   # pairs: a load writes both, pre / post index writes the base
                if (w >> 22) & 1: val.pop(w & 31, None); val.pop((w >> 10) & 31, None)
                if (w >> 23) & 1 or not (w >> 24) & 1: val.pop((w >> 5) & 31, None)
            else:
                if (w & 0x3b000000) == 0x39000000: load = (w >> 22) & 3 != 0   # unsigned offset
                else:
                    load = (w >> 22) & 3 != 0 or (w & 0x3f000000) in (0x08000000, 0x18000000, 0x58000000, 0x98000000)
                    if (w & 0x3b200000) == 0x38000000 and (w >> 10) & 1: val.pop((w >> 5) & 31, None)   # pre / post index
                if load: val.pop(w & 31, None)
            continue
        if (w & 0x1c000000) == 0x14000000 and (w & 0xfff00000) != 0xd5300000: continue   # branches, compares, system (but mrs) write no register
        val.pop(w & 31, None)   # data processing and the rest: Rd

def main():
    a = sys.argv[1:]
    want = []
    if '--sites' in a: i = a.index('--sites'); want = a[i + 1].split(','); del a[i:i + 2]
    if len(a) != 1: sys.exit('usage: loader_census.py <NSO or image> [--sites name,...]')
    img, text = image(a[0])
    n = text // 4; words = struct.unpack_from(f'<{n}I', img, 0)
    def cstr(p):
        e = img.find(b'\0', p, p + 256)
        try: return img[p:e].decode('ascii') if e > p else None
        except UnicodeDecodeError: return None
    slot, loader, page = {}, set(), {}
    for i in range(4, n - 3):
        r = adrp(words[i], i * 4); a = addimm(words[i])
        if r: page[r[0]] = r[1]   # the last address each register was given by adrp (+ add): LÖVE's glad keeps a few globals' addresses in x19–x28 across its loader
        elif a and a[1] in page: page[a[0]] = page[a[1]] + a[2]
        if not is_call(words[i]): continue
        name = None
        for k in range(i - 4, i - 1):   # the name: adrp + add into x0 or x1 just before the call
            r = adrp(words[k], k * 4); s = addimm(words[k + 1])
            if r and s and r[0] in (0, 1) and s[1] == r[0] and s[0] == r[0]: name = cstr(r[1] + s[2])
        if not name: continue
        r = adrp(words[i + 1], (i + 1) * 4); l = ldr64(words[i + 2]); s = str_x0(words[i + 1]); s2 = str_x0(words[i + 2])
        if r and l and l[1] == r[0] and is_str_x0(words[i + 3], l[0]): at = r[1] + l[2]   # through the GOT: adrp + ldr of the slot's address, str x0 to it (Digimon)
        elif r and s2 and s2[0] == r[0]: at = r[1] + s2[1]   # straight into the global: adrp + str x0, [page, #off] (LÖVE's glad: Balatro)
        elif s and s[0] in page and s[0] not in (0, 1): at = page[s[0]] + s[1]   # str x0, [base, #off], base a page set earlier
        else: continue
        if name not in slot.setdefault(at, []): slot[at].append(name)   # a slot two names were stored into keeps both
        loader.update(range((i - 4) * 4, (i + 4) * 4, 4))
    slot = {at: ' / '.join(sorted(names, key=len)) for at, names in slot.items()}
    refs, sites = collections.Counter(), collections.defaultdict(list)
    for i, at in known_loads(words, got_values(img)):
        if at in slot and i * 4 not in loader: refs[slot[at]] += 1; sites[slot[at]].append(i * 4)
    fams = collections.Counter(nm[:2] if nm.startswith('gl') else nm[:3] for nm in slot.values())
    print(f'{len(slot)} slots stored by a loader ({", ".join(f"{k}* {v}" for k, v in fams.most_common(6))}); {len(refs)} loaded by other code:')
    for nm in sorted(refs): print(f'  {refs[nm]:4d}  {nm}')
    for nm in want: print(f'{nm}: ' + ' '.join(hex(x) for x in sites.get(nm, [])))

if __name__ == '__main__': main()
