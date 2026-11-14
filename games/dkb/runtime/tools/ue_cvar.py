#!/usr/bin/env python3
"""Unreal console variables in main found by their registered names, without a build ID: patch_nso.py writes each one's TAutoConsoleVariable offset into main's fixups (UE_CVAR:<name>),
and the host writes the variables the profile decides on (hle_fpslock.c: the frame limit and the present interval, fps_target and ue_fps_cvar; hle_uecvar.c: the texture streaming pool,
ue_texture_pool).
A TAutoConsoleVariable is a global in main's data: its static constructor loads the name (adrp + add), registers it through IConsoleManager (a blr), takes the object's address (adrp + add)
and stores the variable's value pointer at +0x10 (str x0, [xN, #0x10]); the value holds the game thread's copy, then the render thread's. The same sequence registers every variable in the
Unreal games here: OCTOPATH TRAVELER and Yoshi's Crafted World (UE 4.1x, UTF-32 names), Pikmin 4 (4.26), OCTOPATH TRAVELER II and DRAGON QUEST VII Reimagined (4.27, UTF-16), OCTOPATH
TRAVELER 0 (UE 5, a base constructor called between the address and the store) and FINAL FANTASY RESONANCE (5.6). A name registered at two objects gives none, with a [RED] reason.
Usage on its own: ue_cvar.py <module>... (the objects of NAMES in each)"""
import re, struct, sys, os

# The variables the host may write: the engine's frame limit, its present interval (rhi., or nvn. in the Switch RHI of UE 4.18), the texture streaming pool, and a game's own frame-limit
# variable a profile names with ue_fps_cvar (SHIN MEGAMI TENSEI V: Vengeance's Project.FrameRateLimit, which its settings apply to t.MaxFPS). A name a profile names must be listed here
NAMES = ('t.MaxFPS', 'rhi.SyncInterval', 'nvn.SyncInterval', 'r.Streaming.PoolSize', 'Project.FrameRateLimit')
RET = 0xD65F03C0

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w): return (w & 0x9F000000) == 0x90000000
def add_imm(w):   # add Xd, Xn, #imm (64-bit, optionally shifted by 12): (d, n, imm), or None
    if (w & 0xFF800000) != 0x91000000: return None
    return w & 31, (w >> 5) & 31, ((w >> 10) & 0xFFF) << (12 * ((w >> 22) & 1))

def registered_at(word, text_end, j):
    """The object registered under the name whose reference ends at byte offset j (add x1, the name, of the registration's call), or None. The first call after it, with the name still in
    x1, decides: a bl with x0 an address made by adrp + add is the variable's constructor (UE 4.18: TAutoConsoleVariable's, which registers it), and x0 is the object; a blr is
    IConsoleManager's register call (UE 4.2x and 5), and the object is the address made by adrp + add that the value pointer it returns is then stored into, str x0, [xN, #0x10] (UE 5
    calls a base constructor between the two). Anything else (a lookup by name, a log line) is no registration. Registers are followed from the last call or branch before the name (UE
    4.18 loads the object first); those a call or a load overwrites are forgotten, and an adrp x1 after the registration (the next variable's name) ends the search"""
    if not (name_add := add_imm(word(j))) or name_add[0] != 1: return None
    val, name, registered = {}, False, False   # val: register → address made by adrp + add (or copied by mov); name: x1 holds the name
    for t in range(max(0, j - 64), min(text_end, j + 192), 4):
        u = word(t)
        call = (u & 0xFC000000) == 0x94000000 or (u & 0xFFFFFC1F) == 0xD63F0000                       # bl, blr
        if t < j and (call or (u & 0xFC000000) == 0x14000000 or u == RET): val.clear(); continue      # before the name: another sequence
        if call:
            if not registered:
                if not name: return None
                if u >> 26 == 0x25: return val.get(0)                                                  # the variable's constructor
                registered = True
            for r in range(19): val.pop(r, None)                                                       # what the call may overwrite
            name = False; continue
        if (u & 0xFC000000) == 0x14000000 or u == RET: break                                          # b, ret: the constructor ended
        if registered and (u & 0xFFC0001F) == 0xF9000000 and (u >> 10) & 0xFFF == 2 and (u >> 5) & 31 in val: return val[(u >> 5) & 31]   # str x0, [xN, #0x10]
        a, d, v = add_imm(u), u & 31, None   # d: the register the instruction writes, v: the address it then holds (None: unknown)
        if is_adrp(u):
            if registered and d == 1: break                                                            # the next variable's name
            v = adrp_page(u, t)
        elif a: v = val[a[1]] + a[2] if a[1] in val else None
        elif (u & 0xFFE0FFE0) == 0xAA0003E0: v = val.get((u >> 16) & 31)                               # mov xD, xS
        elif (u & 0x0A000000) == 0x08000000:                                                          # loads and stores
            if not (u >> 22) & 1: continue                                                             # a store writes no register
            if (u & 0x3A000000) == 0x28000000: val.pop((u >> 10) & 31, None)                          # ldp: its second register too
        elif (u & 0x1C000000) == 0x14000000: continue                                                 # branches, system instructions
        if d == 1: name = t == j
        if v is None: val.pop(d, None)
        else: val[d] = v
    return None

def find(img, text_end, ro_off, data_off, name):
    """(main's offset of the TAutoConsoleVariable registered under name, reason), or (None, why not); why starts '[RED] ' when the name is registered at more than one object"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    objs = set()
    for z in (2, 4):   # TCHAR: UTF-16 (UE 4.2x and 5), UTF-32 (older Switch builds)
        pat = name.encode('utf-16-le' if z == 2 else 'utf-32-le') + b'\0' * z
        s = img.find(pat, ro_off, data_off)
        while s >= 0:
            if s % z == 0:
                # its references: add xD, xN, #lo (after an adrp xN of its page, at most 5 words before)
                lo = s & 0xFFF
                b1 = b''.join(re.escape(bytes([(lo & 0x3F) << 2 | r])) for r in range(4))   # imm bits 0-5 and the top two bits of Xn
                pat_add = re.compile(rb'(?=[' + b1 + rb']' + re.escape(bytes([lo >> 6])) + rb'\x91)')   # a lookahead: an unaligned match must not hide an aligned one
                for m in pat_add.finditer(img, 1, text_end):
                    j = m.start() - 1
                    if j % 4: continue
                    a = add_imm(word(j))
                    if not a or a[2] != lo: continue
                    if not any(is_adrp(word(i)) and word(i) & 31 == a[1] and adrp_page(word(i), i) == s & ~0xFFF for i in range(max(0, j - 20), j, 4)): continue
                    o = registered_at(word, text_end, j)
                    if o is not None: objs.add(o)
            s = img.find(pat, s + z, data_off)
    if not objs: return None, f'{name}: not registered'
    if len(objs) > 1: return None, f'[RED] {name}: registered at {len(objs)} objects (' + ', '.join(f'{o:#x}' for o in sorted(objs)) + ')'
    o = objs.pop()
    return o, f'{name} at {o:#x}'

def find_all(img, text_end, ro_off, data_off):
    """{name: offset} of the NAMES registered once, and the reasons of those not found (a [RED] one first)"""
    found, why = {}, []
    for n in NAMES:
        o, r = find(img, text_end, ro_off, data_off, n)
        if o is not None: found[n] = o
        else: why.append(r)
    return found, sorted(why, key=lambda r: not r.startswith('[RED] '))

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        found, why = find_all(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t' + ', '.join(f'{n} {o:#x}' for n, o in found.items()) + (' | ' + '; '.join(why) if why else ''))
