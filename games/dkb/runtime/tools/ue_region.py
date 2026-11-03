#!/usr/bin/env python3
"""Unreal Engine 4 / 5's engine address region in main, found by the engine's own code, without a build ID: patch_nso.py puts observation gate OBS_UE_REGION on the store of the
region's size right before nninitStartup reserves it, and the host gives the region what the alias region has room for there (dm/host/hle_svc.c "Unreal's engine region"). The
engine reserves 62 GB by default (16 GB in Unreal Engine 4.18), sized for the Switch's 64 GB alias region. -SwitchVirtualAddressSpaceSize= sets the size, but it is no general way
to give it: UE 4.26's nninitStartup reserves the region only when the option is absent (its size variable is also its "reserved" flag; Pikmin 4), so the base stays 0 with it.

The chain, each link unique or nothing is gated:
- the option's name, "SwitchVirtualAddressSpaceSize" (ASCII), in ro: a main without it has no engine region of this kind;
- nninitStartup, the defined symbol: a b to the engine's function, or that function itself, whose bounds the FDEs give;
- in that function, the one bl to the PLT stub of nn::os::AllocateAddressRegion(uintptr_t *, size_t) whose straight-line code (back to the branch before it) sets x0 to the region's
  base variable (adrp + add, or a mov of one) and stores the size in the word after it, the region's size variable: the last str xS, [xB, #lo + 8] before the call. x1 at the call
  is that size (S = 1, or x1 and xS set from one constant), and nothing after the store writes x1. The gate goes on the store, the register S passed in the gate number's upper bits.
The forms found: a csel of the default and the option's size rounded up to 4 KB, then str x1 (Unreal Engine 4.27 and 5: DRAGON QUEST VII Reimagined, OCTOPATH TRAVELER 0,
FINAL FANTASY RESONANCE, SHIN MEGAMI TENSEI V: Vengeance); the default stored only when the variable is 0, then rounded up into x1 and stored again (OCTOPATH TRAVELER II, 4.27);
str x8 of the default with x1 set from the same constant, reached only when the variable is 0 (Pikmin 4, 4.26), the constant set before the store or after it (TRIANGLE STRATEGY, 4.27:
the rewriter then makes that mov x1, x8, so the request takes the gate's size); str x1 of the 16 GB default, reached only when the variable is 0 (DRAGON QUEST XI S, 4.18). OCTOPATH TRAVELER (4.18 without virtual address memory) has the same code; its nninitStartup never reaches it.
Usage on its own: ue_region.py <module>... (what the chain finds in each)"""
import bisect, re, struct, sys, os
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

OPTION = b'SwitchVirtualAddressSpaceSize'
ALLOCATE = '_ZN2nn2os21AllocateAddressRegionEPmm'

def branch(ins):
    return ins.mnemonic in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.')

def size_store(img, call, start, md):
    """(offset of the size store, its register S, the offset of a later instruction that sets x1 to the same constant, or None) for the bl at call, or a reason it is not the engine region's reservation"""
    a = call
    while a - 4 >= start and a - call > -64:
        if branch(next(md.disasm(img[a - 4:a], a - 4))): break
        a -= 4
    vals, stores, last = {}, [], {}   # vals: register → ('c', constant) | ('page',) | ('lo', the low 12 bits of an address); last: register → where it was last written
    for ins in md.disasm(img[a:call], a):
        ops, m = ins.operands, ins.mnemonic
        reg = lambda o, ins=ins: ins.reg_name(o.reg)
        if m == 'str' and reg(ops[0]).startswith('x') and ops[1].type == ARM64_OP_MEM and not ins.writeback and ops[1].mem.index == 0:   # str xS, [xB, #o]
            stores.append((ins.address, int(reg(ops[0])[1:]) if reg(ops[0]) != 'xzr' else None, ops[1].mem.disp, vals.get(reg(ops[0]))))
            continue
        d = reg(ops[0]) if ops and ops[0].type == ARM64_OP_REG else ''
        v = None   # the value the instruction gives d, read before its writes clear what it reads (add x19, x19, #lo)
        if not d.startswith('x'): pass
        elif m == 'mov' and ops[1].type == ARM64_OP_IMM: v = ('c', ops[1].imm & (1 << 64) - 1)
        elif m == 'mov' and ops[1].type == ARM64_OP_REG: v = vals.get(reg(ops[1]))
        elif m == 'adrp': v = ('page',)
        elif m == 'add' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM and vals.get(reg(ops[1])) == ('page',): v = ('lo', ops[2].imm)
        written = {ins.reg_name(w).replace('w', 'x', 1) for w in ins.regs_access()[1]}
        for x in written: vals.pop(x, None); last[x] = ins.address
        if v: vals[d] = v
    x0 = vals.get('x0')
    if not x0 or x0[0] != 'lo': return None, 'x0 is no base variable'
    stores = [s for s in stores if s[2] == x0[1] + 8]
    if not stores: return None, 'no store of the size beside the base variable'
    off, s, _, val = stores[-1]
    if s is None: return None, 'the size stored is xzr'
    x1_at = last.get('x1')
    if x1_at is not None and x1_at > off:
        # The same constant set again after the store, with S still holding it: patch_nso.py makes that instruction mov x1, xS, so the request takes what the gate gave S
        if val and val[0] == 'c' and vals.get('x1') == val and last.get(f'x{s}', off) <= off: return (off, s, x1_at), None
        return None, f'x1 written after the size store at {off:#x}'
    if s != 1 and (not val or val[0] != 'c' or vals.get('x1') != val): return None, f'x1 is not the x{s} stored at {off:#x}'
    return (off, s, None), None

def plt_stubs(img, text_end, name):
    """Offsets of the PLT stubs of the import name: adrp x16, ldr x17, [x16, #lo], add x16, x16, #lo, br x17 through a GOT slot its R_AARCH64_JUMP_SLOT relocation (DT_JMPREL) names"""
    try:
        mod = struct.unpack_from('<I', img, 4)[0]
        if img[mod:mod + 4] != b'MOD0': return set()
        dyn, tags = mod + struct.unpack_from('<i', img, mod + 4)[0], {}
        for k in range(4096):
            t, v = struct.unpack_from('<qQ', img, dyn + 16 * k)
            if t == 0: break
            tags.setdefault(t, v)
        if tags.get(20) != 7: return set()   # DT_PLTREL: RELA
        slots = set()
        for r in range(tags[23], tags[23] + tags[2], 24):
            slot, info, _ = struct.unpack_from('<QQq', img, r)
            if info & 0xffffffff != 1026: continue
            n = tags[5] + struct.unpack_from('<I', img, tags[6] + (info >> 32) * 24)[0]
            if img[n:img.index(b'\0', n)] == name.encode(): slots.add(slot)
    except (KeyError, ValueError, IndexError, struct.error): return set()
    out, a = set(), -1
    while slots:
        a = img.find(struct.pack('<I', 0xD61F0220), a + 1, text_end)   # br x17
        if a < 0: break
        if a % 4 or a < 12: continue
        p, l, d = struct.unpack_from('<3I', img, a - 12)
        if p & 0x9F00001F != 0x90000010 or l & 0xFFC003FF != 0xF9400211 or d & 0xFFC003FF != 0x91000210: continue
        page = (((((p >> 5) & 0x7FFFF) << 2) | ((p >> 29) & 3)) << 12)
        if page & (1 << 32): page -= 1 << 33
        lo = ((l >> 10) & 0xFFF) * 8
        if (d >> 10) & 0xFFF == lo and ((a - 12) & ~0xFFF) + page + lo in slots: out.add(a - 12)
    return out

def find(img, text_end, ro_off, data_off):
    """(gate site, size register, the instruction to make mov x1, xS or None, reason) when the chain holds; (None, None, None, why not) otherwise, with why starting '[RED] ' when the
    option is there but the reservation is not found (the engine would ask for its whole default)"""
    if img.find(OPTION, ro_off, data_off) < 0: return None, None, None, 'no engine region option'
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from nso_syms import defined
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
    syms = defined(img)
    sym = syms.get('nninitStartup')
    if not sym: return None, None, None, '[RED] Unreal\'s engine region option but no nninitStartup symbol'
    stubs = plt_stubs(img, text_end, ALLOCATE)
    if not stubs: return None, None, None, '[RED] Unreal\'s engine region option but no nn::os::AllocateAddressRegion import'
    word = lambda a: struct.unpack_from('<I', img, a)[0]
    target = lambda a: a + (((word(a) & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4
    # Function starts: the targets of every bl and the defined symbols. A function here runs from its start to the next one (many have no FDE: DRAGON QUEST VII Reimagined's
    # nninitStartup is not among the 32,152 functions its FDEs describe)
    starts = {o for o, n in syms.values() if n and o < text_end}
    for m in re.finditer(rb'[\x94-\x97]', img[:text_end]):
        a = m.start() - 3
        if a % 4 == 0 and word(a) >> 26 == 0x25: starts.add(target(a))
    starts = sorted(starts)
    end = lambda a: starts[bisect.bisect_right(starts, a)] if bisect.bisect_right(starts, a) < len(starts) else text_end
    # nninitStartup is a b to the engine's function (Unreal Engine 4.26 to 5.6) or calls it first (DRAGON QUEST XI S, 4.18): the function itself and those it jumps to or calls
    entry = sym[0]
    fns = [target(entry)] if word(entry) >> 26 == 0x05 else [entry] + [target(a) for a in range(entry, entry + sym[1], 4) if word(a) >> 26 == 0x25]
    found, why = [], []
    for start in fns:
        for p in range(start, end(start), 4):
            if word(p) >> 26 != 0x25 or target(p) not in stubs: continue
            got, no = size_store(img, p, start, md)
            if got: found.append((p, start, *got))
            else: why.append(f'{p:#x}: {no}')
    if len(found) != 1:
        return None, None, None, f'[RED] Unreal\'s engine region: {len(found)} reservations in nninitStartup ({", ".join(f"{f:#x}" for f in fns)})' + ''.join(f'; {x}' for x in why)
    call, start, off, s, x1_mov = found[0]
    return off, s, x1_mov, f'Unreal\'s engine region: reserved at {call:#x} in nninitStartup\'s {start:#x}, its size stored from x{s} at {off:#x}' + (f', x1 set from it at {x1_mov:#x}' if x1_mov is not None else '')

if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    for path in sys.argv[1:]:
        try: m = nso.read(path)
        except nso.Error as e: print(f'{path}\t-\t{e}'); continue
        off, s, x1_mov, why = find(bytes(m.image), m.text_end, m.segments[1][0], m.segments[2][0])
        print(f'{path}\t{m.build_id[:16].hex()}\t{why}')
