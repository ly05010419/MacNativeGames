#!/usr/bin/env python3
"""NVN fetch table and static use surface for a main that reaches its function-pointer slots through the GOT
(Mario Wonder v0 pattern; d2r-mobile dm/gfx/tools/nvn_slots.py handles direct adrp-to-slot code instead).
Loader pattern per name:  adrp/add x1 = name ; blr x19 (GetProcAddress) ; adrp x8 + ldr x8,[x8,#g] = GOT entry ; str x0,[x8] = slot.
GOT entries hold no value in the image: the slot address comes from the module's R_AARCH64_RELATIVE relocations.
Uses: every 'ldr xT,[xN,#imm]' whose base was last written by an adrp, giving one of those GOT entries (backward trace, as in nvn_slots.py),
outside the loader itself; plus direct adrp-to-slot reads.
Usage: nvn_slots_got.py <main raw image (dm/tools/nso.py)> <loader addr> <text end> <out tsv> [names out]"""
import struct, sys

img_path, LOADER, TEXT_END, out = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0), sys.argv[4]
d = open(img_path, 'rb').read()
w = struct.unpack_from('<%dI' % (TEXT_END // 4), d, 0)

def adrp(x, pc):
    imm = ((x >> 29) & 3) | (((x >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20): imm -= 1 << 21
    return (pc & ~0xfff) + (imm << 12)
def cstr(a): return d[a:d.index(b'\0', a)].decode()

# RELATIVE relocations: GOT entry -> slot address
mod0 = struct.unpack_from('<I', d, 4)[0]; dyn = mod0 + struct.unpack_from('<i', d, mod0 + 4)[0]; tags = {}
for i in range(512):
    t, v = struct.unpack_from('<qQ', d, dyn + i * 16)
    if t == 0: break
    tags.setdefault(t, v)
rel = {}
for k in range(tags.get(8, 0) // 24):                            # DT_RELA = 7, DT_RELASZ = 8; a module whose relative relocations are all packed into DT_RELR has neither
    off, info, add = struct.unpack_from('<QQq', d, tags[7] + k * 24)
    if info & 0xffffffff == 1027: rel[off] = add                 # R_AARCH64_RELATIVE
# DT_RELR (36 / 35, from SDK 20): the addend is in the slot; even entry = start, odd entry = bitmap of the next 63 slots
where = None
for k in range(tags.get(35, 0) // 8):
    e = struct.unpack_from('<Q', d, tags[36] + k * 8)[0]
    if not e & 1:
        rel[e] = struct.unpack_from('<Q', d, e)[0]; where = e + 8; continue
    for b in range(63):
        if e >> (b + 1) & 1: rel[where + b * 8] = struct.unpack_from('<Q', d, where + b * 8)[0]
    where += 63 * 8

# 1) loader: name -> GOT entry -> slot
i, reg, got_of_reg, cur, fetched, slots, order = LOADER // 4, {}, {}, None, None, {}, []
while w[i] != 0xd65f03c0:
    x, pc = w[i], i * 4
    if x & 0x9f000000 == 0x90000000: reg[x & 31] = adrp(x, pc); got_of_reg.pop(x & 31, None)
    elif x & 0xffc00000 == 0x91000000 and (x & 31) == 1: cur = cstr(reg[(x >> 5) & 31] + ((x >> 10) & 0xfff))
    elif x & 0xfffffc1f == 0xd63f0000: fetched = cur          # blr: x1 (the name) is consumed here; the result arrives in x0
    elif x & 0xffc00000 == 0xf9400000:                           # ldr xt, [xn, #imm]
        rn, rt = (x >> 5) & 31, x & 31
        if rn in reg: got_of_reg[rt] = reg[rn] + ((x >> 10) & 0xfff) * 8
    elif x & 0xffc00000 == 0xf9000000 and (x & 31) == 0:         # str x0, [xn, #imm]
        rn = (x >> 5) & 31
        if rn in got_of_reg and fetched:
            g = got_of_reg[rn]; s = rel.get(g)
            if s is None: sys.exit('GOT entry %#x has no RELATIVE relocation' % g)
            slots[g] = (fetched, s + ((x >> 10) & 0xfff) * 8); order.append(g); fetched = None
    i += 1
LOADER_END = i * 4
by_slot = {s: g for g, (_, s) in slots.items()}

def writes(x, r):   # same conservative register-write test as nvn_slots.py
    op0 = (x >> 25) & 0xf
    if x & 0xfc000000 == 0x94000000 or x & 0xfffffc1f == 0xd63f0000: return r <= 18
    if op0 in (0b1000, 0b1001, 0b0101, 0b1101): return (x & 31) == r
    if op0 & 0b0101 == 0b0100:
        L = (x >> 22) & 1
        if (x >> 27) & 7 == 0b101:
            wb = (x >> 23) & 3 in (1, 3)
            return (L and ((x & 31) == r or ((x >> 10) & 31) == r)) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111 and (x >> 24) & 3 == 0:
            wb = (x >> 21) & 1 == 0 and (x >> 10) & 3 in (1, 3)
            return (L and (x & 31) == r) or (wb and ((x >> 5) & 31) == r)
        if (x >> 27) & 7 == 0b111: return bool(L) and (x & 31) == r
        if (x >> 27) & 7 == 0b011: return (x & 31) == r
    return False

# 2) uses: reads of a GOT entry (or of the slot itself) outside the loader
uses = {g: [] for g in slots}
offs = {a & 0xfff for a in list(slots) + list(by_slot)}
for i in range(len(w)):
    if LOADER // 4 <= i < LOADER_END // 4: continue
    x = w[i]
    if x & 0xffc00000 != 0xf9400000: continue
    off = ((x >> 10) & 0xfff) * 8; rn = (x >> 5) & 31
    if off & 0xfff not in offs or rn == 31: continue
    for j in range(i - 1, max(i - 4000, -1), -1):
        y = w[j]
        if y == 0xd65f03c0: break
        if writes(y, rn):
            if y & 0x9f000000 == 0x90000000 and (y & 31) == rn:
                a = adrp(y, j * 4) + off
                if a in uses: uses[a].append(i * 4)
                elif a in by_slot: uses[by_slot[a]].append(i * 4)
            break
with open(out, 'w') as f:
    f.write('# kind: evidence  main NVN fetch slots via GOT and static use sites (nvn_slots_got.py)\nname\tslot\tnuses\tfirst_use\n')
    for g in order:
        n, s = slots[g]; u = uses[g]; f.write('%s\t%x\t%d\t%s\n' % (n, s, len(u), '%x' % u[0] if u else '-'))
if len(sys.argv) > 5:
    open(sys.argv[5], 'w').write(''.join(slots[g][0] + '\n' for g in order))
print('loader %#x..%#x: %d names, %d with use sites, %d use sites; %d RELATIVE relocations'
      % (LOADER, LOADER_END, len(slots), sum(1 for g in slots if uses[g]), sum(len(u) for u in uses.values()), len(rel)))
