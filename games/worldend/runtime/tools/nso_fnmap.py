#!/usr/bin/env python3
# Carry offsets across two builds (recover addresses hard-coded in the host / rewriter after a game update): nso_fnmap.py <old .orig.bin> <new .orig.bin> 0xoffset...
# The function table comes from eh_frame (starts + FDE lengths). Each function is hashed over its instructions with address-dependent bits masked: the immediates of b / bl / b.cond / cbz / tbz / adr / adrp / literal ldr,
# and the low 12 bits of an add / ldr / str that follows an adrp and uses its destination register as the base (the data segments moved as a whole, so in-page offsets change too). Struct member offsets and constants still take part.
#   Code offsets: if the containing function has a unique identical hash in the new build → same offset within that function ("exact"); otherwise pick by instruction similarity between the nearest exact matches before / after ("near", similarity reported).
#   Data offsets (globals): the adrp + add/ldr/str pairs in the old build that compute exactly that address are decoded at the same place in the new build when their function is an exact match; several references must agree.
# One line per offset: old → new, method, old / new containing function; reported in red when the instruction at the new place differs (after masking) from the old one.
# The .orig.bin files are produced by dm/tools/nso.py (the whole memory image).
import bisect, collections, difflib, hashlib, struct, sys
import numpy as np

class Img:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        self.w = np.frombuffer(self.b[:len(self.b) // 4 * 4], dtype='<u4')
        m0 = struct.unpack_from('<I', self.b, 4)[0]
        ehs = m0 + struct.unpack_from('<i', self.b, m0 + 16)[0]
        _, penc, cenc, tenc = self.b[ehs:ehs + 4]
        assert (penc, cenc, tenc) == (0x1b, 0x03, 0x3b), f'eh_frame_hdr encodings {penc:#x} {cenc:#x} {tenc:#x}'
        n = struct.unpack_from('<I', self.b, ehs + 8)[0]
        fn = {}
        for i in range(n):
            loc, fde = struct.unpack_from('<ii', self.b, ehs + 12 + i * 8); loc += ehs; fde += ehs
            pc0 = fde + 8 + struct.unpack_from('<i', self.b, fde + 8)[0]; size = struct.unpack_from('<I', self.b, fde + 12)[0]
            if pc0 != loc: continue   # skip FDEs whose encoding is not pcrel sdata4
            fn[loc] = max(fn.get(loc, 0), size)
        self.starts = sorted(fn); self.size = fn
        self.mask = masked(self.w, min(self.starts), max(s + fn[s] for s in self.starts))
        self.hash = {s: hashlib.sha1(self.mask[s // 4:(s + fn[s]) // 4].tobytes()).digest() for s in self.starts}
    def func(self, off):
        i = bisect.bisect_right(self.starts, off) - 1
        return self.starts[i] if i >= 0 and off < self.starts[i] + self.size[self.starts[i]] else None
    def u32(self, off): return int(self.w[off // 4])

def masked(w, lo, hi):
    """mask address-dependent bits word by word within lo..hi (unchanged outside)"""
    m = w.copy(); s = slice(lo // 4, hi // 4); x = m[s]
    for msk, val, keep in ((0x7c000000, 0x14000000, 0xfc000000),   # b / bl
                           (0xff000010, 0x54000000, 0xff00001f),   # b.cond
                           (0x7e000000, 0x34000000, 0xff00001f),   # cbz / cbnz
                           (0x7e000000, 0x36000000, 0xfff8001f),   # tbz / tbnz
                           (0x1f000000, 0x10000000, 0x9f00001f),   # adr / adrp
                           (0x3b000000, 0x18000000, 0xff00001f)):  # literal ldr
        sel = (x & msk) == val; x[sel] &= keep
    # add (64-bit immediate) / ldr / str (unsigned offset) within 8 instructions after an adrp, based on its destination register: mask the low 12 bits
    raw = w[s]; adrp = np.nonzero((raw & 0x9f000000) == 0x90000000)[0]
    for i in adrp:
        rd = int(raw[i]) & 31
        for j in range(i + 1, min(i + 9, len(raw))):
            v = int(raw[j])
            if ((v & 0xff800000) == 0x91000000 or (v & 0x3b000000) == 0x39000000) and (v >> 5) & 31 == rd: x[j] &= ~np.uint32(0x3ffc00); break
            if (v & 31) == rd and (v & 0x9f000000) != 0x90000000 and (v & 0x3b000000) != 0x39000000: break   # destination register rewritten by another instruction
    return m

def adrp_refs(img, target):
    """instruction pairs in img whose adrp + add/ldr/str compute exactly target"""
    out = []; w = img.w; page = target & ~0xfff
    cand = np.nonzero((w & 0x9f000000) == 0x90000000)[0]
    for i in cand:
        v = int(w[i]); imm = ((v >> 29) & 3) | (((v >> 5) & 0x7ffff) << 2)
        if imm & (1 << 20): imm -= 1 << 21
        if ((i * 4) & ~0xfff) + (imm << 12) != page: continue
        rd = v & 31
        for j in range(i + 1, min(i + 9, len(w))):
            u = int(w[j])
            if (u >> 5) & 31 != rd: continue
            if (u & 0xff800000) == 0x91000000: lo = (u >> 10) & 0xfff
            elif (u & 0x3b000000) == 0x39000000: lo = ((u >> 10) & 0xfff) << (u >> 30 if not (u & 0x04800000) == 0x04800000 else 4)
            else: continue
            if page + lo == target: out.append((i * 4, j * 4))
            break
    return out

def pair_addr(img, a, b):
    v, u = img.u32(a), img.u32(b)
    imm = ((v >> 29) & 3) | (((v >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20): imm -= 1 << 21
    lo = (u >> 10) & 0xfff
    if (u & 0x3b000000) == 0x39000000: lo <<= (u >> 30 if not (u & 0x04800000) == 0x04800000 else 4)
    return (a & ~0xfff) + (imm << 12) + lo

def main():
    old, new = Img(sys.argv[1]), Img(sys.argv[2])
    by_new = collections.defaultdict(list)
    for s, h in new.hash.items(): by_new[h].append(s)
    by_old = collections.Counter(old.hash.values())
    exact = {s: by_new[h][0] for s, h in old.hash.items() if by_old[h] == 1 and len(by_new.get(h, ())) == 1}
    print(f'functions old {len(old.starts)} new {len(new.starts)}, unique exact matches {len(exact)}')
    ex_old = sorted(exact)
    def near(f):
        if f in exact: return exact[f], 1.0, 'exact'
        i = bisect.bisect_left(ex_old, f); lo = exact[ex_old[i - 1]] if i else 0; hi = exact[ex_old[i]] if i < len(ex_old) else 1 << 62
        a = old.mask[f // 4:(f + old.size[f]) // 4].tolist(); best = (0, None)
        for g in new.starts[bisect.bisect_right(new.starts, lo):bisect.bisect_left(new.starts, hi)]:
            if abs(new.size[g] - old.size[f]) > max(64, old.size[f] // 4): continue
            r = difflib.SequenceMatcher(None, a, new.mask[g // 4:(g + new.size[g]) // 4].tolist(), autojunk=False).ratio()
            if r > best[0]: best = (r, g)
        return best[1], best[0], f'near {best[0]:.3f}'
    for arg in sys.argv[3:]:
        off = int(arg, 16); f = old.func(off)
        if f is not None:
            g, r, how = near(f)
            if g is None: print(f'[RED] {off:#x}: function {f:#x} not found in the new build'); continue
            n = g + off - f; same = old.mask[off // 4] == new.mask[n // 4]
            print(f'{"" if same and r == 1.0 else "[RED] " if not same else ""}{off:#x} → {n:#x}  code  {how}  old fn {f:#x}+{off - f:#x} → new {g:#x}' + ('' if same else f'  instruction differs {old.u32(off):08x} / {new.u32(n):08x}'))
            continue
        refs = adrp_refs(old, off); got = collections.Counter(); used = 0
        for a, b in refs:
            fa = old.func(a)
            if fa is None or fa not in exact: continue
            g = exact[fa]; got[pair_addr(new, g + a - fa, g + b - fa)] += 1; used += 1
        if not got: print(f'[RED] {off:#x}: data, {len(refs)} references in the old build, none in an exact-match function'); continue
        (n, k), = got.most_common(1)
        print(f'{"" if len(got) == 1 else "[RED] "}{off:#x} → {n:#x}  data  {k}/{used} references agree ({len(refs)} in the old build)' + ('' if len(got) == 1 else f'  disagree {dict(got)}'))

if __name__ == '__main__': main()
