#!/usr/bin/env python3
"""Unreal Engine 4 / 5's FMallocBinned3 in any module, found by what the engine itself holds, without a build ID: patch_nso.py cuts the call of its constructor and puts observation
gate OBS_UE_MALLOC on that call, and the host gives the object its own allocator's vtable there (dm/host/hle_uemalloc.c). The constructor reserves 37 GB of address space for its
pools (74 of 512 MB), which an iPhone cannot give.

The chain, each link unique or nothing is cut:
- the allocator's name, the UTF-16 string "Binned3" (TEXT("Binned3"); it starts a string), in ro;
- its getter, GetDescriptiveName: adrp x0 + add x0 of the string, then ret;
- the one data word holding the getter (the module's relative relocations applied), a slot of the vtable: the slots run back to two words of 0 (offset to top, no RTTI). The getter's
  slot number names the slot layout: 19 for Unreal Engine 4.26 and 4.27, 22 for UE 5, 26 for UE 5.6 (hle_uemalloc.c); any other stops the chain with a [RED] line;
- the constructor: the one function other than the vtable's two destructors that stores the vtable's address (adrp + add, then str to [xN]);
- its one call site, a bl (no b to it): x0 there is the object, already made (GCreateMalloc mallocs it, or a bss object in nninitStartup).
Checked on the five Unreal 4 / 5 mains on the game drive (OCTOPATH TRAVELER 0 and II, DRAGON QUEST VII Reimagined, Pikmin 4, FINAL FANTASY RESONANCE), which gave the call sites of their
registered cuts. Unreal Engine 3 (Batman: Arkham Knight) inlines the constructor; its cut stays its build's (its patches.py).
Usage on its own: ue_malloc.py <module>... (what the chain finds in each)"""
import re, struct, sys, os

LAYOUTS = {19: 'Unreal Engine 4.26 / 4.27', 22: 'Unreal Engine 5', 26: 'Unreal Engine 5.6'}   # the getter's slot → the slot layouts of hle_uemalloc.c
NAME = 'Binned3'.encode('utf-16-le') + b'\0\0'
RET, NOP = 0xD65F03C0, 0xD503201F

def relocated(img):
    """{offset: value} of the image's relative relocations (RELA R_AARCH64_RELATIVE and RELR, whose addend is in place); a word not in it holds its value in place"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from loader_census import got_values
    return got_values(img)

def adrp_page(w, pc):
    imm = ((((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return (pc & ~0xFFF) + imm
def is_adrp(w, rd=None): return (w & 0x9F000000) == 0x90000000 and (rd is None or w & 31 == rd)

def find(img, text_end, ro_off, data_off):
    """(call site, vtable, the getter's slot, reason) when the chain holds; (None, None, None, why not) otherwise, with why starting '[RED] ' when the allocator is there but cannot
    be replaced (its pools would take the address space)"""
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    s = -1; names = []
    while True:
        s = img.find(NAME, s + 1, data_off)
        if s < 0: break
        if s >= ro_off and s % 2 == 0 and (s == ro_off or img[s - 2:s] == b'\0\0'): names.append(s)
    if not names: return None, None, None, 'no FMallocBinned3 name'
    if len(names) > 1: return None, None, None, f'[RED] {len(names)} FMallocBinned3 names: ' + ', '.join(f'{x:#x}' for x in names)
    s = names[0]
    tail = struct.pack('<II', 0x91000000 | (s & 0xFFF) << 10, RET)   # add x0, x0, #lo; ret
    getters, a = [], -1
    while True:
        a = img.find(tail, a + 1, text_end)
        if a < 0: break
        if a % 4 == 0 and a >= 4 and is_adrp(word(a - 4), 0) and adrp_page(word(a - 4), a - 4) == s & ~0xFFF: getters.append(a - 4)
    if len(getters) != 1: return None, None, None, f'[RED] FMallocBinned3 name at {s:#x}: {len(getters)} getters'
    g = getters[0]
    rel = relocated(img)
    value = lambda a: rel.get(a, struct.unpack_from('<Q', img, a)[0])
    slots = [o for o, v in rel.items() if v == g and o >= data_off]
    a = data_off - 8
    while True:   # values in place (a module without relative relocations, or RELR)
        a = img.find(struct.pack('<Q', g), a + 8)
        if a < 0: break
        if a % 8 == 0 and a not in slots: slots.append(a)
    if len(slots) != 1: return None, None, None, f'[RED] FMallocBinned3 getter at {g:#x}: {len(slots)} vtable slots'
    is_code = lambda v: 0 < v < text_end and v % 4 == 0
    vt = slots[0]
    while vt - 8 >= data_off and is_code(value(vt - 8)): vt -= 8
    slot = (slots[0] - vt) // 8
    if (value(vt - 16), value(vt - 8)) != (0, 0): return None, None, None, f'[RED] FMallocBinned3 vtable at {vt:#x}: no offset to top and RTTI words of 0 before it'
    if slot not in LAYOUTS: return None, None, None, f'[RED] FMallocBinned3 vtable at {vt:#x}: the name getter is slot {slot}, a slot layout the host does not know'
    dtors = {value(vt), value(vt + 8)}
    # Direct calls and jumps in the text: bl (top bits 100101) and b (000101), by target
    bl, b = {}, {}
    for m in re.finditer(rb'[\x94-\x97\x14-\x17]', img[:text_end]):
        a = m.start() - 3
        if a % 4: continue
        w = word(a); t = a + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4
        (bl if w >> 26 == 0x25 else b).setdefault(t, []).append(a)
    # Stores of the vtable's address: add xD, xN, #lo (after an adrp xN of its page), then str xD, [xM]
    lo = vt & 0xFFF
    pat = re.compile(rb'(?=(?s:..)' + re.escape(bytes([lo >> 6])) + rb'\x91)')   # a lookahead: an unaligned match must not hide an aligned one
    ctors = set()
    for m in pat.finditer(img[:text_end]):
        a = m.start()
        if a % 4: continue
        w = word(a)
        if (w >> 10) & 0xFFF != lo: continue
        n, d = (w >> 5) & 31, w & 31
        if not any(is_adrp(word(p), n) and adrp_page(word(p), p) == vt & ~0xFFF for p in range(max(0, a - 32), a, 4)): continue
        if not any((word(q) & 0xFFFFFC1F) == 0xF9000000 | d for q in range(a + 4, min(text_end, a + 48), 4)): continue   # str xD, [xM]
        start = max((t for t in list(bl) + list(b) + list(dtors) if t <= a), default=None)
        if start is None or any(word(q) == RET for q in range(start, a, 4)): return None, None, None, f'[RED] FMallocBinned3 vtable at {vt:#x}: no function start for its store at {a:#x}'
        if start not in dtors: ctors.add(start)
    if len(ctors) != 1: return None, None, None, f'[RED] FMallocBinned3 vtable at {vt:#x}: {len(ctors)} constructors (' + ', '.join(f'{x:#x}' for x in sorted(ctors)) + ')'
    c = ctors.pop()
    if len(bl.get(c, [])) != 1 or b.get(c): return None, None, None, f'[RED] FMallocBinned3 constructor at {c:#x}: {len(bl.get(c, []))} calls and {len(b.get(c, []))} jumps, not one call'
    return bl[c][0], vt, slot, f'FMallocBinned3 ({LAYOUTS[slot]}): its constructor at {c:#x}, called at {bl[c][0]:#x}; its vtable {vt:#x}, the name getter slot {slot}'

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        call, vt, slot, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
