#!/usr/bin/env python3
"""A target frame rate the game steps by, found in main by the code that reads and sets it, without a build ID: patch_nso.py writes its place into main's fixups (FPS_TARGET_PTR, the
global pointer's offset, and FPS_TARGET_FIELD, the field's offset in the object it points to), and the host writes the profile's fps_target there (hle_fpslock.c).
The field is an int32 frame rate in an object a global points to. Its readers load it into a float register, convert it and divide 60.0 by it: the vsyncs one frame stands for, by which
they step the game (ldr sN, [xB, #F]; scvtf sC, sN; fdiv sD, s60, sC, the 60.0 made by movz wK, #0x4270, lsl #16 and fmov). The game sets it with an immediate 30 or 60 (str wK, [xB, #F]).
The object's address is loaded from the global each time: ldr xB, [xA], xA made by adrp + add (the global) or adrp + ldr (its GOT slot, the module's relative relocations applied).
A field with at least MIN_READERS such readers and a store of 30 is the target; exactly one must be, or nothing is written (two give a [RED] line).
Found on Fire Emblem Fortune's Weave v131072 (Intelligent Systems' engine, its gxc graphics object; 59 readers, 30 set where gameplay starts and 60 for the title and menus, and its present
asks for interval 60 / target). Checked on 100 mains on the drives (2026-11-03): it finds 55 of those 59 readers, and not one reader in any other main, Paper Mario: The Origami King
and The Thousand-Year Door (Intelligent Systems' older engine) among them.
Usage on its own: fps_target.py <module>... (what it finds in each)"""
import struct, sys, os

# A reader is the division of 60.0 by the field. Fortune's Weave has 59; a lone division of 60 by an int field (a rate in a menu, a timer) must not count as the game's step
MIN_READERS = 8
SIXTY = 0x42700000   # 60.0f

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000
def movz_w(w):   # movz wD, #imm{, lsl #16}: (d, value), or None
    if (w & 0xFF800000) != 0x52800000: return None
    return w & 31, ((w >> 5) & 0xFFFF) << (16 * ((w >> 21) & 1))

def writes(u):   # the general register an instruction writes, roughly: Rd of anything but stores, branches and FP / SIMD loads (None)
    if (u & 0x0A000000) == 0x08000000:   # loads and stores
        if not (u >> 22) & 1 or (u >> 26) & 1: return None   # a store, or a load into an FP / SIMD register
        return u & 31
    if (u & 0x1C000000) == 0x14000000: return None   # branches, system instructions
    if (u & 0x1F000000) == 0x1E000000 and (u >> 29) & 1 == 0 and (u & 0xFFFFFC00) not in (0x1E260000, 0x9E660000): return None   # FP data processing (fmov wD, sN writes a general one)
    return u & 31

def relocated(img):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

def find(img, text_end, ro_off, data_off):
    """(offset of the global pointer, offset of the field, reason), or (None, None, why not); why starts '[RED] ' when more than one field qualifies"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    rel = None
    def page_of(i, a):
        """The page adrp xA made a few words before word offset i, with xA written by nothing in between, or None"""
        for k in range(i - 4, max(0, i - 24), -4):
            u = word(k)
            if is_adrp(u) and u & 31 == a: return adrp_page(u, k)
            if writes(u) == a: return None
        return None
    def global_of(i, b):
        """The global whose value xB holds at word offset i (ldr xB, [xA] a few words before, xA the global's address), or None"""
        nonlocal rel
        for k in range(i - 4, max(0, i - 40), -4):
            u = word(k)
            if u & 31 == b and (u & 0xFFC00000) == 0xF9400000:   # ldr xB, [xA, #imm]
                if (u >> 10) & 0xFFF: return None
                a = (u >> 5) & 31
                for k2 in range(k - 4, max(0, k - 32), -4):
                    v = word(k2)
                    if v & 31 != a: continue
                    if (v & 0xFFC00000) == 0x91000000 and (v >> 5) & 31 == a:   # add xA, xA, #lo
                        page = page_of(k2, a)
                        return page + ((v >> 10) & 0xFFF) if page is not None else None
                    if (v & 0xFFC00000) == 0xF9400000 and (v >> 5) & 31 == a:   # ldr xA, [xA, #got]: the GOT slot
                        page = page_of(k2, a)
                        if page is None: return None
                        if rel is None: rel = relocated(img)
                        return rel.get(page + ((v >> 10) & 0xFFF) * 8)
                    return None
                return None
            if writes(u) == b: return None   # xB written by something else
        return None
    readers, stores = {}, {}
    for i in range(0, text_end, 4):
        u = word(i)
        if (u & 0xFFC00000) == 0xBD400000:   # ldr sT, [xB, #F]
            t, b, f = u & 31, (u >> 5) & 31, ((u >> 10) & 0xFFF) * 4
            conv = next((k for k in range(i + 4, min(text_end, i + 24), 4) if (word(k) & 0xFFFFFFE0) == 0x5E21D800 | t << 5), None)   # scvtf sC, sT
            if conv is None: continue
            c = word(conv) & 31
            div = next((k for k in range(conv + 4, min(text_end, conv + 20), 4) if (word(k) & 0xFFE0FC00) == 0x1E201800 and (word(k) >> 16) & 31 == c), None)   # fdiv sD, sM, sC
            if div is None: continue
            m = (word(div) >> 5) & 31
            fm = next((k for k in range(div - 4, max(0, div - 48), -4) if (word(k) & 0xFFFFFC1F) == 0x1E270000 | m), None)   # fmov sM, wK
            if fm is None or not any((z := movz_w(word(k))) and z == ((word(fm) >> 5) & 31, SIXTY) for k in range(fm - 4, max(0, fm - 32), -4)): continue
            g = global_of(i, b)
            if g is not None and g >= data_off: readers.setdefault((g, f), []).append(i)
        elif (u & 0xFFC00000) == 0xB9000000:   # str wK, [xB, #F]
            k_, b, f = u & 31, (u >> 5) & 31, ((u >> 10) & 0xFFF) * 4
            val = next((z[1] for k in range(i - 4, max(0, i - 32), -4) if (z := movz_w(word(k))) and z[0] == k_), None)
            if val not in (30, 60): continue
            g = global_of(i, b)
            if g is not None and g >= data_off: stores.setdefault((g, f), []).append((i, val))
    found = [(g, f) for (g, f), r in readers.items() if len(r) >= MIN_READERS and any(v == 30 for _, v in stores.get((g, f), ()))]
    if not found:
        best = max(readers.items(), key=lambda x: len(x[1]), default=None)
        return None, None, 'no target frame rate' + (f' (most readers: {len(best[1])} of [{best[0][0]:#x}]+{best[0][1]:#x}, stores {[v for _, v in stores.get(best[0], ())]})' if best else '')
    if len(found) > 1: return None, None, '[RED] target frame rate: ' + ', '.join(f'[{g:#x}]+{f:#x}' for g, f in found) + ' qualify'
    g, f = found[0]
    st = stores[(g, f)]
    return g, f, f'target frame rate at [{g:#x}]+{f:#x}: {len(readers[(g, f)])} readers of 60 / target, set to ' + ', '.join(f'{v} at {a:#x}' for a, v in st)

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        g, f, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
