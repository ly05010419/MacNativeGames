# Generic NSO rewriter: Switch aarch64 module → an image that can execute natively on Apple systems.
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Rewrites (the Apple kernel modifies x18 and tpidr_el0; Horizon's thread area, svc and timers have no counterpart on Apple):
#   x18 read/write      → thread slot K18 (TSD, via tpidrro_el0)
#   mrs tpidr_el0       → read thread slot KTLS; msr tpidr_el0 → write KTLS
#   mrs tpidrro_el0     → read thread slot KTLR (Horizon ThreadLocalRegion pointer)
#   svc #n, mrs cntpct_el0 → host gate (gate address taken from thread slot KGATE; once in C, gates.tsv is looked up by return point)
#   sub sp, sp, xN      → xN rounded up to 16 first (Apple's kernel faults on a misaligned sp, Horizon does not; site_sp_align)
#   spin wait loops     → a copy whose back-edges pass a spin gate (the host yields there; a busy poll costs nothing on the Switch, see "Spin waits")
#   Registered cuts (CUTS of the patch files, restricted by NSO build ID, see "Registered patches"): before rewriting, the original bytes are asserted one by one and any mismatch aborts; results are listed in <name>.cuts.tsv
#   Registered observation gates (OBS of the patch files, restricted by build ID): one instruction at the entry is replaced with "enter the host observation callback → execute the original instruction → jump back", so diagnostics modify zero code at runtime; listed in <name>.obs.tsv
# The key page only stores "actual key number × 8" at the end of the data segment, and patch code fetches thread slots indirectly through the key page: the loader may use any key number.
# Output: <name>.patched.bin (key page and patch area appended after the image at PATCH_OFF; when that would start beyond 112 MB the patch area sits before the base, see FAR), <name>.fixups.tsv (layout and key page index header), <name>.gates.tsv, <name>.sites.tsv, <name>.cuts.tsv, <name>.obs.tsv
import struct, re, sys, os, glob, types, importlib.util
from itertools import combinations
import lz4.block
from capstone import *
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
import nso_asm
from nso_asm import *   # the instruction encoders (B, ADRP, LDRX, ...), shared with the per-build patch files

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
d = open(src, 'rb').read()
if d[0x10:0x14] == b'NRO0':
    # An NRO (a module the game loads at run time with nn::ro, Super Smash Bros. Ultimate's fighter code): the file is the image itself, each segment stored at its memory offset
    # and uncompressed; +0x20 text, +0x28 ro, +0x30 data as {offset, size}, +0x38 bss size, +0x40 the build ID. Rewritten as an NSO of the same layout would be
    flags, csz = 0, [struct.unpack_from('<II', d, 0x20 + 8 * i)[1] for i in range(3)]
    segs = [(o, o, sz) for o, sz in (struct.unpack_from('<II', d, 0x20 + 8 * i) for i in range(3))]
    bss = struct.unpack_from('<I', d, 0x38)[0]
else:
    assert d[:4] == b'NSO0'
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    bss = struct.unpack_from('<I', d, 0x3c)[0]
    csz = struct.unpack_from('<III', d, 0x60)
# Every jump between the patch area and the code is a B (±128 MB). A patch area appended after the image (with bss) cannot reach the code when it would start beyond 112 MB (leaving 16 MB for the patch area): Metroid Dread's main has about 1.8 GB of bss (the engine's static heap); Engage's main starts its patch area at about 103 MB and keeps it after the image.
# The patch area then sits FRONT bytes before the module base (PATCH_OFF is negative), and the output file holds only text + ro + data (FILE_IMAGE_LEN) followed by the patch area; the loader supplies zero pages for bss and the key page (IMAGE_SIZE is the whole image).
# The key page stays after bss (the patch area reaches it through ADRP, ±4 GB). Modules below the limit produce output byte-identical to before
NEAR_PATCH_OFF = ((((segs[2][1] + segs[2][2] + bss + 0x3FFF) & ~0x3FFF) + 0x4000 + 0xFFFF) & ~0xFFFF)   # where an appended patch area would start (as in the else branch below)
FAR, FRONT = NEAR_PATCH_OFF > 0x7000000, 0x2000000
# A text segment that ends within 32 MB of the ±128 MB reach (Monster Hunter Rise: 125.3 MB of text) puts the patch area right before the base instead. Its stubs follow the sites in address
# order, so the last site's stub ends up about as far from it as the text is long. The first pass tries the gap that keeps every site in reach; when the patch area does not fit there
# (Monster Hunter Rise: 3.6 MB), the rewriter runs again with exactly the size it needs (DM_PATCH_FRONT, set only by that rerun) and B() checks every branch
BIG_TEXT = FAR and segs[0][2] + FRONT > 0x8000000 - 0x10000
if BIG_TEXT: FRONT = int(os.environ.get('DM_PATCH_FRONT', '0'), 0) or (0x8000000 - 0x10000 - segs[0][2]) & ~0xFFFF
IMG = bytearray(segs[2][1] + segs[2][2] + (0 if FAR else bss))
for i, (fo, mo, sz) in enumerate(segs):
    raw = d[fo:fo + csz[i]]
    if flags >> i & 1: raw = lz4.block.decompress(raw, uncompressed_size=sz)
    IMG[mo:mo + sz] = raw
TEXT_END = segs[0][2]
RO_OFF, DATA_OFF = segs[1][1], segs[2][1]
if FAR:
    FILE_IMAGE_LEN = (len(IMG) + 0x3FFF) & ~0x3FFF
    KEY_PAGE_OFF = (len(IMG) + bss + 0x3FFF) & ~0x3FFF
    IMAGE_SIZE, PATCH_OFF = KEY_PAGE_OFF + 0x4000, -FRONT
else:
    KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
    IMG.extend(bytes(0x4000))
    PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF

# ---- Registered patches of particular builds, keyed by NSO build ID: dm/tools/sdk_patches.py (Nintendo SDK module builds, which several games can ship) and dm/games/<id>/patches.py
# (a game's own modules: its profile's builds). Every file is loaded, whatever the game, so a module gets the patches of its build wherever it comes from; a build ID claimed twice stops the rewrite.
# A file may define:
#   CUTS = {build ID: [(offset, original bytes, new bytes, reason)]}: before rewriting, the original bytes are asserted one by one and any mismatch aborts (listed in <name>.cuts.tsv)
#   OBS = {build ID: [(offset, number, original instruction word, reason)]}: observation gates. Numbers match OBS_* in the host's hle.h; the original instruction must be position
#     independent (no PC-relative / branch / anything involving x18 or tpidr), asserted offline; when the host has no callback attached the gate block goes in and out once and returns
#   FRAME_STEP = {build ID: function(ctx)}: the frame step stubs of a main ("Frame step in whole vsyncs" below)
PATCH_FILES = [os.path.join(os.path.dirname(os.path.abspath(__file__)), 'sdk_patches.py')] + sorted(glob.glob(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games/*/patches.py')))
for games_dir in filter(None, os.environ.get('DM_GAMES_DIR', '').split(os.pathsep)):   # provisional / derived profiles outside the repository
    PATCH_FILES += sorted(glob.glob(os.path.join(games_dir, '*/patches.py')))
def load_patches():
    tables, where = {'CUTS': {}, 'OBS': {}, 'FRAME_STEP': {}}, {}
    for n, path in enumerate(PATCH_FILES):
        spec = importlib.util.spec_from_file_location(f'patches_{n}', path); m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        for kind, table in tables.items():
            for bid, v in getattr(m, kind, {}).items():
                assert bid not in table, f'{kind} of build {bid} in both {where[kind, bid]} and {path}'
                table[bid], where[kind, bid] = v, path
    return tables, where
PATCHES, PATCH_WHERE = load_patches()
BUILD_ID = d[0x40:0x50].hex()
cuts = list(PATCHES['CUTS'].get(BUILD_ID, []))
for off, ob, nb, why in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'cut original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
# Key page indices are fixed; the key page content is written by the loader as "actual key number × 8".
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511}
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_OFF + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

# ---- Encoders: dm/tools/nso_asm.py (imported above) ----

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except Exception: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(0, 16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """x18/w18 → xS/wS: only the 4 register field positions are changed, verified against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

# ---- Rewrite blocks of each kind ----
def sp_delta(ins):
    """The amount an instruction writes back to sp (pre-index [sp, #i]! / post-index [sp], #i); returns 0 if sp is not written back."""
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # Instructions that modify sp write below sp, so the scratch save is placed deep in the reserved area
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # Always write back: capstone can miss reporting a write (the adds w18 instance)
    emit(LDP(S, T, 31, save - dlt)); emit(B(here(), ins.address + 4))
    return start

def site_sp(ins):
    """Destination is sp and a source is x18 (Engage's alloca: mov x18, sp; sub x18, x18, x0; mov sp, x18). site_plain saves its scratch relative to the old sp and would restore it relative to the new one:
    copy the old sp into T first, and after the instruction restore S / T relative to T (without writeback, ldp may use the base register as a destination). x18 is not written, so the slot is not written back"""
    S, T = pick(regs_in(ins))
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(ADDI(T, 31, 0))             # mov T, sp (in add immediate, 31 is sp)
    emit(subst(ins, S))
    emit(LDP(S, T, T, -16)); emit(B(here(), ins.address + 4))
    return start

def site_sp_align(ins, n):
    """sub sp, sp, xN (no shift): Horizon leaves stack alignment checking off for user code, Apple's kernel does not, so an sp that is not 16-byte aligned faults (SIGBUS) at the next sp-based access.
    Paper Mario: The Thousand-Year Door's script call bridges (main+0x17c140 and five more beside it, hand-written) reserve the callee's stack arguments as their sizes add up (4 for one int)
    and give the area back with add sp, sp, xN: the callee's first stp faulted. xN is rounded up to 16 first, which the add then gives back too. Compiled code keeps such sizes 16-aligned, so
    there it changes nothing; of the 22 games' modules on hand only Paper Mario's main has the form"""
    start = here()
    emit(ADDI(n, n, 15)); emit(0x927CEC00 | n << 5 | n)   # add xN, xN, #15; and xN, xN, #~15
    emit(struct.unpack('<I', ins.bytes)[0]); emit(B(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(B(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')   # S first serves as a temporary for the key page offset; the next instruction then loads the target value
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(B(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """Jump table br x18: target = base + 4i. Stash x16 in the red zone → equally spaced landing pads → restore x16 → original target."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 are temporary registers that can stay live within a function; save them to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi: h = min(hi, 0xFFF); ops.append(op(16, 16, h, 1)); hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(B(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)      # ldur x16,[sp,#-16]
        emit(B(here(), base + 4 * i))
    return start

def site_br_tab(ins, targets):
    """Offset-table br x18 (ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB): target = table base + signed entry; the table is constant, so all targets are read at rewrite time.
    x16 = guest x18, compared with each target in turn (adrp + add put the target address in x17); on a match restore x16 / x17 and jump to the original target; no match → brk (the original bounds check already stops out-of-range indices)"""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # as in site_br: save x16 / x17 to the red zone before borrowing
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    uniq = sorted(set(targets))
    pads = here() + 4 * (4 * len(uniq) + 1)
    for k, t in enumerate(uniq):
        emit(ADRP(17, here(), t)); emit(ADDI(17, 17, t & 0xFFF)); emit(CMPR(16, 17)); emit(BEQ(here(), pads + 12 * k))
    emit(BRK(0x18))
    for t in uniq:
        emit(LDUR(16, 31, -16)); emit(LDUR(17, 31, -24)); emit(B(here(), t))
    return start

def site_blr(ins):
    """blr x18 (Engage main+0x9ff324): x16 = guest x18, x30 = the instruction after the original blr, br x16. At a call site x16 / x17 are intra-procedure-call temporaries under the calling convention, so they need no saving;
    the return address is the original one, so the callee returns straight there (IL2CPP's C++ exceptions look up unwind tables by return address and must not stop in the patch area)"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    ret = ins.address + 4
    emit(ADRP(30, here(), ret)); emit(ADDI(30, 30, ret & 0xFFF)); emit(BR(16))
    return start

def site_br_call(ins):
    """br x18 as an indirect tail call (tail_call): x16 = guest x18, br x16. As at site_blr, x16 / x17 are intra-procedure-call temporaries at a call, and x30 keeps the caller's return address"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(BR(16))
    return start

def site_sysreg_read(ins, kind):
    """mrs xN, tpidr_el0 / tpidrro_el0 → xN = thread slot value."""
    N = ins.bytes[0] & 31
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))   # tmp is saved by borrowing the red zone
    emit(B(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)           # stur xT,[sp,#-16]
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)           # ldur xT,[sp,#-16]
    emit(B(here(), ins.address + 4))
    return start

def emit_gate_call():
    """Common section of a gate block (right after stp x29,x30,[sp,#-16]! and before blr x30): x30 = the gate address in thread slot KGATE.
        Fetching the slot through the key page needs a second register to be borrowed; x29 is borrowed and then immediately restored from the stack: the x29 saved by _dm_gate must be
        the guest's original value, since the crash backtrace walks the x29 chain."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """svc / mrs cntpct_el0 → host gate. The gate address is taken from thread slot KGATE; the gate looks up gates.tsv by return point (the address of this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(B(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """Observation gate: original entry instruction → enter the host gate (arg = observation number) → restore x29/x30 → execute the original instruction → jump back to off+4.
        The first half of the gate block is word-for-word isomorphic with site_gate; the host only reads registers and does not modify them (the x0–x8 written back by dm_gate are the original values)."""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'observation gate original instruction cannot be relocated {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(B(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

# ---- Exclusive windows ----
EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        # Another exclusive load before the store: this one has no store of its own (a retry block, see retry_block); spanning both would merge two sequences
        # (Tears of the Kingdom main+0x5a4674: ldxr w18; cmp; b.eq <stxr of 0x5a427c>; ...; 0x5a469c: ldxr w13; and; stxr; cbnz, a loop other code branches to)
        if any(x.mnemonic in EXCL_LD for x in insl[i + 1:j]): continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def site_window(seg):
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    # Exit stubs: the first stub right after the window must be the fall-through one (after the last window instruction, hi); targets of branches leaving the window come after it.
    # Previously the fall-through stub came last: as soon as a window contains a conditional branch out (e.g. ldxr; cmp; b.ne out; stxr; cbnz), a successful stxr fell into the
    # first branch-out stub, i.e. took the "compare not equal" path (SMBW main 0x81c40: the enqueue retried forever, the tail index filled up without storing a single task, and the job system hung). D2R's windows had no branch out, so it never hit this.
    exits = copy0 + 4 * n + 16
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else: ex_targets.append(tgt); newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in [hi] + ex_targets:   # fall-through first
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start, lo, hi

def retry_block(insl, k, wseg):
    """The branch insl[k] lands in the middle of window wseg. Accepted only when it ends an out-of-line retry block that re-enters the window at its store-exclusive:
    an exclusive load, then instructions that do not branch, then this branch. Tears of the Kingdom main+0x18598: ldxr w17,[x10]; cmp w15,w17; b.eq 0x181fc,
    where 0x181fc is the stxr w17,w18,[x10] of the window 0x181ec..0x18200, whose failed stxr branches back to 0x18598 (same shape at 0x185e8 → 0x18244).
    Returns (the retry block, the window from that store on), or None"""
    t = insl[k].operands[-1].imm
    tail = [y for y in wseg if y.address >= t]
    if not tail or tail[0].mnemonic not in EXCL_ST: return None
    for p in range(k - 1, max(-1, k - 8), -1):
        q = insl[p]
        if q.mnemonic in EXCL_LD: return insl[p:k + 1], tail
        if q.mnemonic in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or q.mnemonic.startswith('b.') or q.mnemonic in EXCL_ST: return None
    return None

def site_retry_window(parts):
    """A retry block (see retry_block) gets a patch copy of its own: it cannot jump into the window's copy, because the x18 stand-in would have to be loaded
    between its exclusive load and the store. parts = [(retry block, its fall-through), (window from the store on, the window's fall-through)] are copied one
    after the other under one prologue, each followed by its fall-through stub; branches to any copied instruction (between the parts, and the failed store's
    branch back to the retry block's head) stay inside the copy, every other target leaves through an exit stub as in site_window"""
    used = set()
    for seg, _ in parts:
        for x in seg: used |= regs_in(x)
    S, T = pick(used)
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    pos, at = {}, here()
    for seg, _ in parts:
        for x in seg: pos[x.address] = at; at += 4
        at += 16
    exits, ex_targets = at, []
    for seg, ft in parts:
        for x in seg:
            w = struct.unpack('<I', x.bytes)[0]
            if has18(x): w = subst(x, S)
            m, pc = x.mnemonic, here()
            if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
                tgt = x.operands[-1].imm
                if tgt not in pos and tgt not in ex_targets: ex_targets.append(tgt)
                newt = pos[tgt] if tgt in pos else exits + 16 * ex_targets.index(tgt)
                off = (newt - pc) >> 2
                if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
                else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
                dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
            else:
                assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
            emit(w)
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), ft)); emit(NOP)
    for t in ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(B(here(), t)); emit(NOP)
    return start

# ---- Function boundaries (eh_frame_hdr) ----
m0 = struct.unpack_from('<I', IMG, 4)[0]
assert IMG[m0:m0 + 4] == b'MOD0'
ehs = m0 + struct.unpack_from('<i', IMG, m0 + 16)[0]
def rd(o, enc):
    fmt, app = enc & 0xF, enc & 0x70
    v, sz = {0x3: (struct.unpack_from('<I', IMG, o)[0], 4), 0xB: (struct.unpack_from('<i', IMG, o)[0], 4),
             0x4: (struct.unpack_from('<Q', IMG, o)[0], 8)}[fmt]
    if app == 0x10: v += o
    if app == 0x30: v += ehs
    return v, sz
_, penc, cenc, tenc = IMG[ehs:ehs + 4]
o = ehs + 4
_, s = rd(o, penc); o += s
nfde, s = rd(o, cenc); o += s
starts = set()
for _ in range(nfde):
    a, s = rd(o, tenc); o += s
    _, s = rd(o, tenc); o += s
    starts.add(a)
starts = sorted(x for x in starts if x < TEXT_END)

# ---- Linear scan to find all rewrite points (not relying on register read/write reports) ----
# Data blocks in the text (nso_code.py: Absolum's main starts with 1.7 MB of them) are not code: words there that read as rewrite sites are left alone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_code
DATA_BLOCKS = nso_code.data_blocks(IMG, TEXT_END, md)
CODE_RANGES = list(zip([0] + [e for _, e in DATA_BLOCKS], [s for s, _ in DATA_BLOCKS] + [TEXT_END]))
md_lite = Cs(CS_ARCH_ARM64, CS_MODE_ARM)   # no details: the x18 scan reads only the operand text
R18 = re.compile(r'\b[xw]18\b')
def x18_words(lo, hi):
    """offsets in [lo, hi) of the instructions whose operands name x18 / w18 (as has18). Decoded 16 KB at a time without details, going on after a word capstone cannot decode:
    a third of the time of decoding one word at a time with details (Paper Mario: The Thousand-Year Door main, 437 sites in 300,000 words either way)"""
    out, a = set(), lo
    while a < hi:
        end = min(hi, a + 0x4000); last = a - 4
        for addr, size, mn, op in md_lite.disasm_lite(bytes(IMG[a:end]), a):
            last = addr
            if R18.search(op): out.add(addr)
        a = last + 4 if last + 4 >= end else last + 8   # the chunk is done, or the word after the last decoded one is no instruction
    return out
x18 = set().union(*(x18_words(s, e) for s, e in CODE_RANGES))
kinds = {}
# Spin wait candidates (see "Spin waits" below): backward direct branches (b, b.cond, cbz / cbnz, tbz / tbnz) at most SPIN_WIN bytes back, by target
SPIN_WIN, SPIN_TOPS = 512, {0x16, 0x17, 0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7}
spin_cand = {}
for o in (o for s, e in CODE_RANGES for o in range(s, e, 4)):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w >> 24 in SPIN_TOPS:
        t = w >> 24
        if t in (0x16, 0x17): d = ((w & 0x3FFFFFF) - (1 << 26)) * 4
        elif t & 0x7E == 0x36: d = (((w >> 5) & 0x3FFF) - (1 << 14)) * 4 if w & 0x40000 else 4
        elif t == 0x54: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 and not w & 0x10 else 4
        else: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 else 4
        if -SPIN_WIN <= d <= 0: spin_cand.setdefault(o + d, []).append(o)
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # mrs xN, ctr_el0 (cache type): the Apple kernel does not expose it to user mode (SIGILL); SDK 16's nn::os::FlushDataCache reads it for the cache line size
    if w & 0xFFFFFFE0 == 0xD53B0020: kinds[o] = ('ctr', w & 31); continue
    # sub sp, sp, xN (extended register, LSL #0): sp may end up misaligned, which Horizon allows and Apple's kernel does not (site_sp_align); with x18 it is an x18 site (site_sp)
    if w & 0xFFE0FFFF == 0xCB2063FF and (w >> 16) & 31 != 18: kinds[o] = ('sp_align', (w >> 16) & 31); continue
    if o in x18: kinds[o] = ('x18', None)

# Take the instruction table per function, find the windows and jump tables
import bisect
def func_of(a):
    """The function holding a: from the FDE start at or before it to the next one. Code before the first FDE is one function as well, from the end of the data block before it (or the text's
    start) to the first FDE: FINAL FANTASY RESONANCE 1.0.1 main (Unreal Engine 5.6.1) has no unwind entry before main+0x6a75d4, and each of its sites there used to be a function of one
    instruction, so the byte jump table at main+0x219234 (adr x18; ldrb w0,[x17,x16]; add x18,x18,x0,lsl #2; br x18) was never seen. Of the 120 modules of 33 games on hand, only one other has x18
    sites before its first FDE (DRAGON QUEST VII Reimagined main, 110 before +0xb310, none in an exclusive window or a br x18), so every other rewrite is byte-identical"""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0: return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END)
    return (max((e for s, e in DATA_BLOCKS if e <= a), default=0), starts[0]) if starts else (a, a + 4)
done = set(); windows = []; jumps = {}; tabjumps = {}; tailcalls = set(); retries = []
def check_before_copy(insl, ctx, ld, idx):
    """The index copied from the checked register before the check, far back (DRAGON QUEST XI S main+0x2dd8cd0: mov w0,w18, then 41 instructions with two other jump tables on x0, then
    cmp w18,#4; b.hi; ldrsw x18,[x11,x0,lsl #2]; add x18,x18,x11; br x18). Accepted when the cmp and its b.hi come right before the load and the mov is the last writer of the index and of the
    checked register before them (64 instructions back at most). The compiler checks one register and indexes with the other only when it knows they are equal; blocks in between that branch out
    and back (main+0x2dd8ce0 → main+0x2dd8cb8) write neither. Tried only when every other form has failed (jump_offsets copy_first), so sites that rewrote before come out the same:
    the one br x18 of its kind among the 25,508 x18 sites of that main. → the cmp, or None"""
    k = ctx.index(ld)
    if k < 2 or ctx[k - 1].mnemonic != 'b.hi' or ctx[k - 2].mnemonic != 'cmp': return None
    m = re.fullmatch(r'(w\d+), #(0x[0-9a-f]+|\d+)', ctx[k - 2].op_str)
    if not m: return None
    src, kl = m.group(1), insl.index(ld)
    def writes(p):   # capstone lists a compare's first operand as written (cmp w18,#4 at main+0x2dd8c30); it only sets the flags
        return set() if p.mnemonic in ('cmp', 'cmn', 'tst') else {'w' + p.reg_name(r)[1:] for r in p.regs_access()[1]}
    for q in range(kl - 3, max(0, kl - 64) - 1, -1):
        p = insl[q]
        mm = re.fullmatch(r'(w\d+), (w\d+)', p.op_str) if p.mnemonic == 'mov' else None
        if mm and mm.group(1) in idx and mm.group(2) == src: break
        if writes(p) & (idx | {src}): return None
    else: return None
    return ctx[k - 2]
def jump_offsets(insl, j, copy_first=False, far_base=False):
    """The br x18 is preceded by the offset-table form (Engage main+0x12eec94): cmp w18,#N; b.hi; ldrsw x18,[xB,x18,lsl #2]; add x18,x18,xB.
    The index may be another register, and the bounds check may be on a register copied into it: Link's Awakening's GLSLC (subsdk0+0x1be6e8) has
    sub w0,w18,#0xd1; cmp w0,#0x31; b.hi; ldrsw x18,[x17,x0,lsl #2], its movie module (subsdk1+0xae9a4) cmp w21,#7; b.hi; ...; mov w18,w21; ldrsw x18,[x4,x18,lsl #2].
    The entry may also be loaded into another register and only the add writes x18: Cuphead (Unity 2017.4, main+0x1bb872c) has cmp w17,#7; ldrsw x17,[x15,x17,lsl #2]; add x18,x17,x15
    (found by armyja, GitHub pull request #1).
    For the table base xB, search back for the nearest writer: it must be add xB,xB,#lo, and the writer before it must be adrp xB. With far_base, a base that nothing writes in those
    256 instructions was set further back, and its writer chain is followed to the function's start (table_base far), every target an instruction of the function: ASTRAL CHAIN main+0x6db498
    and +0x6db70c load bases set 441 and 549 instructions before, x24 in the function's first block and x3 after it, which the loop around their dispatches keeps. A function-wide search
    for the one adrp + add pair that fits (table_cands, as jump_adr_offsets does) finds two for x3 there, the other a table of its own further on. Returns all targets, or None if not recognized"""
    ctx = insl[max(0, j - 16):j]
    add = next((p for p in reversed(ctx) if p.mnemonic == 'add' and re.fullmatch(r'x18, x\d+, x\d+', p.op_str)), None)
    if not add: return None
    _, t, rb = add.op_str.split(', ')   # t = the loaded entry, rb = the table base
    ld = next((p for p in reversed(ctx) if p.mnemonic == 'ldrsw' and re.fullmatch(rf'{t}, \[{rb}, x(\d+), lsl #2\]', p.op_str)), None)
    if not ld: return None
    # GLSLC subsdk0+0x2f403c checks a masked copy instead: and w16,w18,#0x3f; cmp w16,#0xe; b.hi; ...; and x18,x18,#0x3f; ldrsw x18,[x15,x18,lsl #2]
    idx = {'w' + re.fullmatch(rf'{t}, \[{rb}, x(\d+), lsl #2\]', ld.op_str).group(1)}
    masked = None   # (source, mask, position) when the index is "source & mask"
    k0 = ctx.index(ld); cmp = None
    def wname(r): return 'w' + r[1:]
    for i in range(k0 - 1, -1, -1):   # walk back from the load: follow register copies into the index, stop at the bounds check
        p = ctx[i]
        m = re.fullmatch(r'(w\d+), (w\d+)', p.op_str) if p.mnemonic == 'mov' else None
        if m and m.group(1) in idx: idx.add(m.group(2)); continue
        m = re.fullmatch(r'([wx]\d+), ([wx]\d+), #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == 'and' else None
        if m and wname(m.group(1)) in idx and masked is None: masked = (wname(m.group(2)), int(m.group(3), 0), i); continue
        m = re.fullmatch(r'(w\d+), #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == 'cmp' else None
        if not m: continue
        if m.group(1) in idx: cmp = p; break
        if masked:   # the checked register must be "the same source & the same mask", with the source unchanged up to the index's and
            for q in range(i - 1, -1, -1):
                w = [wname(ctx[q].reg_name(r)) for r in ctx[q].regs_access()[1]]
                if m.group(1) not in w: continue
                mm = re.fullmatch(r'([wx]\d+), ([wx]\d+), #(0x[0-9a-f]+|\d+)', ctx[q].op_str) if ctx[q].mnemonic == 'and' else None
                if mm and wname(mm.group(2)) == masked[0] and int(mm.group(3), 0) == masked[1] and \
                   not any(masked[0] in [wname(ctx[z].reg_name(r)) for r in ctx[z].regs_access()[1]] for z in range(q + 1, masked[2])):
                    cmp = p
                break
            if cmp: break
    if not cmp and copy_first: cmp = check_before_copy(insl, ctx, ld, idx)
    if not cmp: return None
    n = int(cmp.op_str.split('#')[1], 0) + 1
    k = insl.index(ld); lo = page = None
    for p in reversed(insl[max(0, k - 256):k]):   # the writer chain of the table base
        _, w = p.regs_access()
        if not any(p.reg_name(r) == rb for r in w): continue
        if lo is None and p.mnemonic == 'add' and re.fullmatch(rf'{rb}, {rb}, #(0x[0-9a-f]+|\d+)', p.op_str): lo = int(p.op_str.split('#')[1], 0); continue
        if lo is not None and p.mnemonic == 'adrp' and p.op_str.startswith(rb + ','): page = p.operands[1].imm
        break
    else:
        if lo is None and far_base:   # nothing writes the base in the window
            tb = table_base(insl, k, rb, far=True)
            if tb is None: tb = copied_base(insl, k, rb)   # built further back and copied on its way (below)
            if tb is None or tb + 4 * n > len(IMG): return None
            t = [tb + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)]
            fa, fb = insl[0].address, insl[-1].address + 4
            return t if all(fa <= x < fb and x % 4 == 0 for x in t) else None
    if page is None and far_base:
        # The base was built further back and passed through other registers on its way (Super Smash Bros. Ultimate 13.0.5 main+0x2b74458: adrp x7 + add x7,x7,#0x40c
        # 688 instructions before, then mov x30,x7 ... mov x7,x30 around a loop), so the walk back meets a mov. Follow the copies back to the function's start (copied_base);
        # every target must be an instruction of this function. A function-wide search for the one adrp + add pair (table_cands) finds two for x7 there, the other a table of its own
        # further on. Only the last attempt (far_base) gets here, i.e. only a site the rewriter could not handle before
        tb = copied_base(insl, k, rb)
        if tb is None or tb + 4 * n > len(IMG): return None
        t = [tb + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)]
        fa, fb = insl[0].address, insl[-1].address + 4
        return t if all(fa <= x < fb and x % 4 == 0 for x in t) else None
    if page is None: return None
    tb = page + lo
    return [tb + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)]
def table_base(insl, k, rb, far=False):
    """Address the table-base register rb holds at insl[k]: its writer chain before k must be add rb,rb,#lo after adrp rb (as in jump_offsets); None otherwise.
    The chain is searched 256 instructions back, or with far to the function's start, through no call when rb is caller-saved (capstone lists only x30 as a call's write; a call between
    means the code before it in address order is not the path that set rb)"""
    lo = None
    for p in reversed(insl[0 if far else max(0, k - 256):k]):
        if far and p.mnemonic in ('bl', 'blr') and num(rb) <= 18: return None
        _, w = p.regs_access()
        if not any(p.reg_name(r) == rb for r in w): continue
        if lo is None and p.mnemonic == 'add' and re.fullmatch(rf'{rb}, {rb}, #(0x[0-9a-f]+|\d+)', p.op_str): lo = int(p.op_str.split('#')[1], 0); continue
        if lo is not None and p.mnemonic == 'adrp' and p.op_str.startswith(rb + ','): return p.operands[1].imm + lo
        return None
    return None
def copied_base(insl, k, rb):
    """Address rb holds at insl[k] when it was built by adrp + add and then only copied between registers (mov xD, xS): the writer chain is walked back in address order to the
    function's start, switching to the source register at each copy, through no call while the register followed is caller-saved; None for any other writer"""
    canon = {'lr': 'x30', 'fp': 'x29'}   # capstone's reg_name calls x30 lr and x29 fp
    lo, r = None, canon.get(rb, rb)
    for p in reversed(insl[:k]):
        if p.mnemonic in ('bl', 'blr') and num(r) <= 18: return None
        _, w = p.regs_access()
        # A write to wN zeroes the upper half of xN too; it is a clobber, never a copy of the 64-bit base.
        if not any(canon.get(p.reg_name(x), p.reg_name(x)).replace('w', 'x', 1) == r for x in w): continue
        ops = [canon.get(o, o) for o in p.op_str.split(', ')]
        if lo is None and p.mnemonic == 'mov' and len(ops) == 2 and ops[0] == r and re.fullmatch(r'x\d+', ops[1]): r = ops[1]; continue
        if lo is None and p.mnemonic == 'add' and len(ops) == 3 and ops[:2] == [r, r] and re.fullmatch(r'#(0x[0-9a-f]+|\d+)', ops[2]): lo = int(ops[2][1:], 0); continue
        if lo is not None and p.mnemonic == 'adrp' and ops[0] == r: return p.operands[1].imm + lo
        return None
    return None
def jump_adr_offsets(insl, j):
    """Entries relative to an adr rather than to the table (Tears of the Kingdom main+0x393bb0): cmp w0,#5; b.hi; ...; adr x18,#L; ldrsw x1,[x17,x0,lsl #2];
    add x18,x18,x1; br x18 → target = L + entry, with the table base x17 from adrp + add. Tried only where jump_offsets finds nothing. Returns all targets, or None if not recognized"""
    ctx = insl[max(0, j - 16):j]
    add = next((p for p in reversed(ctx) if p.mnemonic == 'add' and re.fullmatch(r'x18, x\d+, x\d+', p.op_str)), None)
    if not add: return None
    ka = ctx.index(add); ops = add.op_str.split(', ')[1:]
    for e, a in (ops, ops[::-1]):   # e = the loaded entry, a = the adr base
        ld = next((p for p in reversed(ctx[:ka]) if p.mnemonic == 'ldrsw' and re.fullmatch(rf'{e}, \[x\d+, x\d+, lsl #2\]', p.op_str)), None)
        wa = next((p for p in reversed(ctx[:ka]) if any(p.reg_name(r) == a for r in p.regs_access()[1])), None)   # the nearest writer of a
        if not ld or not wa or wa.mnemonic != 'adr': continue
        tb_reg, ix = re.fullmatch(rf'{e}, \[(x\d+), x(\d+), lsl #2\]', ld.op_str).groups()
        idx, cmp = {'w' + ix}, None   # the index and the registers copied into it (main+0x596d30: cmp w15,#8; b.hi; mov w16,w15; ...; ldrsw x0,[x17,x16,lsl #2])
        for p in reversed(ctx[:ctx.index(ld)]):   # walk back from the load to the bounds check
            m = re.fullmatch(r'(w\d+), (w\d+)', p.op_str) if p.mnemonic == 'mov' else None
            if m and m.group(1) in idx: idx.add(m.group(2)); continue
            m = re.fullmatch(r'(w\d+), #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == 'cmp' else None
            if m and m.group(1) in idx: cmp = p; break
            if any('w' + p.reg_name(r)[1:] in idx for r in p.regs_access()[1]): return None   # the index changes after the check
        if cmp is None: return None
        n, at = int(cmp.op_str.split('#')[1], 0) + 1, wa.operands[1].imm
        def targets(tb): return [at + struct.unpack_from('<i', IMG, tb + 4 * i)[0] for i in range(n)] if 0 <= tb and tb + 4 * n <= len(IMG) else None
        tb = table_base(insl, insl.index(ld), tb_reg)
        if tb is not None: return targets(tb)
        # The base register was loaded further away (main+0x5f2a9c: hoisted out of the dispatch block): take every adrp + add pair in the function that builds it,
        # and accept the table only if exactly one candidate makes every target an instruction of this function
        fa, fb = insl[0].address, insl[-1].address + 4
        ok = [tb for tb in table_cands(insl, tb_reg) if (t := targets(tb)) and all(fa <= x < fb and x % 4 == 0 for x in t)]
        return targets(ok[0]) if len(ok) == 1 else None
    return None
def table_cands(insl, rb):
    """Every address an adrp rb + add rb,rb,#lo pair in the function builds, sorted"""
    cands = set()
    for k, p in enumerate(insl):
        if p.mnemonic != 'adrp' or not p.op_str.startswith(rb + ','): continue
        q = next((x for x in insl[k + 1:k + 9] if any(x.reg_name(r) == rb for r in x.regs_access()[1])), None)
        m = re.fullmatch(rf'{rb}, {rb}, #(0x[0-9a-f]+|\d+)', q.op_str) if q is not None and q.mnemonic == 'add' else None
        if m: cands.add(p.operands[1].imm + int(m.group(1), 0))
    return sorted(cands)
def tail_call(insl, j):
    """The br x18 is an indirect tail call, not a jump table: the last write of x18 before it is a plain pointer load and the br ends the function. Ace Attorney Investigations Collection
    (Unity 2022.3 IL2CPP) main+0x1d271bc: ldr x18,[x0,#0x18]; ...; ldr x16,[sp]; ...; br x18, a thunk that moves stack arguments and jumps to the method pointer at +0x18"""
    if j + 1 != len(insl): return False
    for p in reversed(insl[:j]):
        if 'x18' not in [p.reg_name(r) for r in p.regs_access()[1]]: continue
        return p.mnemonic == 'ldr' and bool(re.fullmatch(r'x18, \[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', p.op_str))
    return False
def jump_bytes_far(insl, j):
    """The byte / halfword table form with its add further back (Octopath Traveler 0 main+0x37f9bc8: adr x18,#L; ldrb w0,[x17,x15]; add x18,x18,x0,lsl #2, then 17 register moves before br x18).
    Up to 32 instructions back, and x18 must not change between the add and the br. Tried only where the 16-instruction search and the offset forms find nothing. Returns (base, width) or None"""
    ctx = insl[max(0, j - 32):j]
    for k in range(len(ctx) - 1, -1, -1):
        p = ctx[k]
        if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str: break
        if any(p.reg_name(r) == 'x18' for r in p.regs_access()[1]): return None
    else: return None
    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
    base = next((q.operands[1].imm for q in reversed(ctx[:k]) if q.mnemonic == 'adr' and q.op_str.startswith('x18,')), None)
    ld = next((q for q in reversed(ctx[:k]) if q.mnemonic in ('ldrb', 'ldrsb', 'ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx), None)
    if base is None or ld is None: return None
    return base, 256 if ld.mnemonic in ('ldrb', 'ldrsb') else half_count(insl, ld, base) or 65536
def half_count(insl, ld, base):
    """Landing pads a halfword table needs (ldrh wE,[xT,xI,lsl #1]; ...; adr x18,#L; add x18,x18,xE,lsl #2; br x18: target = L + 4 × entry). site_br gives every possible entry a pad,
    65,536 of them (768 KB), whatever the switch's size: Animal Crossing: New Horizons 3.0.3 main has 51 such tables, 40 MB, which put its patch area's end beyond the ±128 MB a B reaches
    (a B() assertion at its spin waits), and Tears of the Kingdom's main has 11. The table is read as jump_adr_offsets reads one: the index's bounds check (following register copies),
    then the table base from adrp + add, or the one candidate in the function whose every target is an instruction of it → the largest entry + 1, or None (all 65,536, as before).
    Byte tables keep their 256 pads (3 KB)"""
    m = re.fullmatch(r'w\d+, \[(x\d+), [wx](\d+), (?:lsl|uxtw) #1\]', ld.op_str) if ld.mnemonic == 'ldrh' else None
    if not m: return None
    tb_reg, k = m.group(1), insl.index(ld)
    idx, cmp = {'w' + m.group(2)}, None
    for p in reversed(insl[max(0, k - 32):k]):   # walk back from the load to the bounds check
        mm = re.fullmatch(r'(w\d+), (w\d+)', p.op_str) if p.mnemonic == 'mov' else None
        if mm and mm.group(1) in idx: idx.add(mm.group(2)); continue
        mm = re.fullmatch(r'(w\d+), #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == 'cmp' else None
        if mm and mm.group(1) in idx: cmp = p; break
        if any('w' + p.reg_name(r)[1:] in idx for r in p.regs_access()[1]): return None   # the index changes after the check
    kc = insl.index(cmp) if cmp is not None else -1
    if kc < 0 or insl[kc + 1].mnemonic not in ('b.hi', 'b.hs', 'b.cs'): return None   # only a compare that guards the dispatch bounds the index (b.hs: one entry more than needed)
    n = int(cmp.op_str.split('#')[1], 0) + 1
    fa, fb = insl[0].address, insl[-1].address + 4
    def entries(tb): return struct.unpack_from(f'<{n}H', IMG, tb) if 0 <= tb and tb + 2 * n <= len(IMG) else None
    def fits(e): return e is not None and all(fa <= base + 4 * x < fb for x in e)
    tb = table_base(insl, k, tb_reg)
    if tb is None:
        ok = [c for c in table_cands(insl, tb_reg) if fits(entries(c))]
        tb = ok[0] if len(ok) == 1 else None
    e = entries(tb) if tb is not None else None
    return max(e) + 1 if fits(e) else None
def disasm_fn(fa, fb):
    """A function's instructions. Capstone stops at the first word it cannot decode, so go on after it: Monster Hunter Rise main+0x6b34540 is a trap word (0xe7ffdefe)
    in the middle of a function, and the br x18 jump table after it (main+0x6b346d0) was never seen"""
    out, a = [], fa
    while a < fb:
        l = list(md.disasm(bytes(IMG[a:fb]), a))
        out += l; a = (l[-1].address + 4 if l else a) + 4
    return out
# Text with no unwind entry is one function to func_of. FINAL FANTASY RESONANCE 1.0.1 main (Unreal Engine 5.6.1) has 82 MB of it (main+0x6c5008 to +0x59220c0) beside the 6.65 MB before its
# first FDE: disassembled with details at once, it took 100 GB of memory and the rewrite never finished on a 16 GB Mac. A function longer than BIG_FN is analysed in pieces of about PIECE
# bytes, cut only right after an unconditional ret / br / b where no conditional branch and no b within ±PIECE_B crosses: a window, a jump table's context (the bounds check's branch
# and the table base before it) and a retry block reach each other by fall-through or by such branches, so each lies within one piece. Farther b / bl that cross a cut (calls and
# tail calls) are checked once every piece is done: none may land inside a window or a retry block (far_into). With b up to 1 MB counted, tail calls between neighbouring functions left
# 21,385 cut points in that 82 MB and pieces of up to 6.5 MB (about 4 GB while disassembled); with 64 KB, 185,228 points and none over PIECE
BIG_FN, PIECE, PIECE_B = 0x40000, 0x40000, 0x10000
def pieces(fa, fb):
    """[(start, end)] covering [fa, fb), and the far b / bl in it as [(site, target)]"""
    from array import array
    n = (fb - fa) // 4
    w = memoryview(bytes(IMG[fa:fa + 4 * n])).cast('I')
    cross = array('i', bytes(4 * (n + 2))); far = []
    for i in range(n):
        x = w[i]; t = x >> 24
        if t == 0x54 and not x & 0x10 or t & 0x7E == 0x34: d = (x >> 5) & 0x7FFFF; d -= (d & 0x40000) << 1   # b.cond, cbz / cbnz
        elif t & 0x7E == 0x36: d = (x >> 5) & 0x3FFF; d -= (d & 0x2000) << 1                                  # tbz / tbnz
        elif t & 0x7C == 0x14:                                                                                # b, bl
            d = x & 0x3FFFFFF; d -= (d & 0x2000000) << 1
            if t & 0x80 or 4 * abs(d) > PIECE_B: far.append((fa + 4 * i, fa + 4 * (i + d))); continue
        else: continue
        lo, hi = max(min(i, i + d), -1), min(max(i, i + d), n)   # the branch crosses the cut before instruction k when lo < k <= hi
        cross[lo + 1] += 1; cross[hi + 1] -= 1
    cand, c = [], 0
    for k in range(n):
        c += cross[k]
        x = w[k - 1] if k else 0
        if k and not c and (x & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000) or x >> 26 == 0x05): cand.append(k)   # after ret / br / b, nothing crossing
    cuts, s = [], 0
    while s + PIECE // 4 < n:
        j = bisect.bisect_right(cand, s + PIECE // 4) - 1
        if j < 0 or cand[j] <= s: j = bisect.bisect_right(cand, s + PIECE // 4)
        if j >= len(cand): break
        s = cand[j]; cuts.append(s)
    b = [0] + cuts + [n]
    return [(fa + 4 * p, fa + 4 * q) for p, q in zip(b, b[1:])], far
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
far_into, rmid_all = [], set()
if any(fb - fa > BIG_FN for fa, fb in funcs):
    sites18 = sorted(a for a, (k, _) in kinds.items() if k == 'x18'); split = []
    for fa, fb in funcs:
        if fb - fa <= BIG_FN: split.append((fa, fb)); continue
        ps, far = pieces(fa, fb); far_into += far
        split += [(p, q) for p, q in ps if bisect.bisect_left(sites18, p) < bisect.bisect_left(sites18, q)]   # the pieces that hold x18 sites
        print(f'function {fa:#x}..{fb:#x} ({(fb - fa) / 2**20:.2f} MB, no unwind entries) analysed in {len(ps)} pieces')
    funcs = split
for fa, fb in funcs:
    insl = disasm_fn(fa, fb)
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            # 16 instructions back, as for the offset-table form: Hades main+0x1135d0 has six vector moves between the add and the br, which put its adr x18 nine back
            ctx = insl[max(0, j - 16):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'): base = p.operands[1].imm; break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    # capstone names x30 'lr' (and x29 'fp'): Age of Calamity main+0x1382160 indexes with add x18,x18,x30,lsl #2 after ldrb w30,[x14,x12]
                    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = half_count(insl, q, base) or 65536 if base else 65536
                        if width: break
                    break
            if not (base and width):
                tgt = jump_offsets(insl, j) or jump_adr_offsets(insl, j)
                if not tgt and tail_call(insl, j): tailcalls.add(ins.address); continue
                if not tgt and (far := jump_bytes_far(insl, j)): jumps[ins.address] = far; continue
                if not tgt: tgt = jump_offsets(insl, j, copy_first=True, far_base=True)   # the index copied before the check (check_before_copy) or the base set far back, only where every other form failed
                assert tgt, f'jump table {ins.address:x}'
                tabjumps[ins.address] = tgt; continue
            jumps[ins.address] = (base, width)
    # No jump may land in the middle of a window, except a retry block's branch onto the window's store-exclusive (retry_block), which gets a copy of its own
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    rmid = set()
    for k, x in enumerate(insl):
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            if t in inwin and t not in heads:
                wseg = next(w for w in ws if any(y.address == t for y in w))
                rb = retry_block(insl, k, wseg) if x.mnemonic not in ('b', 'bl') else None
                assert rb, f'branch into window {x.address:x}'
                assert not any(y.address in done for y in rb[0]), f'retry block overlaps a window {rb[0][0].address:x}'
                retries.append([(rb[0], x.address + 4), (rb[1], wseg[-1].address + 4)])
                done |= {y.address for y in rb[0]}; rmid |= {y.address for y in rb[0][1:]}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            assert x.operands[-1].imm not in rmid, f'branch into retry block {x.address:x}'
    for a_, tgt in tabjumps.items():
        if fa <= a_ < fb: assert not any(t in inwin and t not in heads for t in tgt), f'jump table target into window {a_:x}'
    rmid_all |= rmid
if far_into:   # functions analysed in pieces (pieces above): their far b / bl, and every jump table's targets (a cut may follow a br x18), may not land inside a window or a retry block
    win_mid = {x.address for wseg in windows for x in wseg[1:]}
    for at, to in far_into: assert to not in win_mid and to not in rmid_all, f'far branch into a window or retry block {at:x}'
    for at, tgt in tabjumps.items(): assert not any(t in win_mid or t in rmid_all for t in tgt), f'jump table target into a window or retry block {at:x}'

def put_branch(at, to): struct.pack_into('<I', IMG, at, B(at, to))
for wseg in windows:
    st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for parts in retries:
    st = site_retry_window(parts); stat('retry_window'); put_branch(parts[0][0][0].address, st)
for a in sorted(kinds):
    if a in done: continue
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br' and a in tailcalls: st = site_br_call(ins); stat('br_call')
        elif m == 'br': st = site_br_tab(ins, tabjumps[a]) if a in tabjumps else site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m == 'blr': st = site_blr(ins); stat('blr')
        elif re.match(r'(sp|wsp),', ins.op_str): st = site_sp(ins); stat('sp')
        elif m in ('ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct'): st = site_gate(ins, k, arg); stat(k)
    elif k == 'sp_align': st = site_sp_align(ins, arg); stat(k)
    elif k == 'ctr':   # replaced in place by a constant: DminLine = 4 (64-byte lines); movz xN, #4, lsl #16
        assert arg != 18, f'{a:x} mrs x18, ctr_el0'
        struct.pack_into('<I', IMG, a, 0xD2A00080 | arg); stat(k); continue
    put_branch(a, st)

def scratch_dead(off):
    """x9–x17 carry nothing at off: from off on, every one of them is written before it is read, up to a bl (a call clobbers them all; DRAGON QUEST VII Reimagined's gate sits on the
    mov x0, x19 before one). Only straight-line code is followed: a branch, a blr or a ret before the bl answers no"""
    written = set()
    for a in range(off, min(off + 4 * 16, TEXT_END), 4):
        ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
        if ins.mnemonic == 'bl': return True
        if ins.mnemonic in ('b', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or ins.mnemonic.startswith('b.'): return False
        rd, wr = regset(ins)
        if any(9 <= (num(r) or -1) <= 17 and num(r) not in written for r in rd): return False
        written |= {num(r) for r in wr}
    return False

obs = list(PATCHES['OBS'].get(BUILD_ID, []))
for off, oid, ow, why in obs:
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'observation gate original instruction mismatch {off:#x}: {w:08x}'
    # Function entries, or the instruction right after a bl, only: dm_gate writes back only x0–x8 / x30, and host C code may modify x9–x17; at entry these carry no values under AAPCS64 (arguments are only in x0–x7 / x8),
    # and right after a call returns the callee was free to clobber them anyway (x16 / x17 are the veneer registers). Elsewhere, where the code ahead reads none of them before its next bl
    after_bl = off >= 4 and struct.unpack_from('<I', IMG, off - 4)[0] >> 26 == 0x25
    assert off in set(starts) or after_bl or scratch_dead(off), f'observation gate neither at a function entry, after a bl nor before a call with x9–x17 unread {off:#x}'
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'observation gate conflicts with a rewrite site {off:#x}'
    put_branch(off, site_obs(off, oid, w)); stat('obs')

# ---- Build-gated frame timing (AoC 1.3.0 main) ----
# The profile's FRAME_STEP callback asserts original instructions and emits its
# native timing stubs into the patch area. The writable state page follows the
# key page. AoC's target, dynamic-step mode, bounded elapsed interval and clock
# counters are described in its patches.py and gfx/nvn/aoc_timing.h. The loader
# rejects older state ABIs before executing guest code.
# ctx.here/emit assemble words; ctx.site asserts the complete displaced bytes
# and absence of other rewrite sites; ctx.put records each cut and its reason.
FRAME_STEP = PATCHES['FRAME_STEP']
FS_STATE_OFF = 0
RENDER_SCALE = 0   # 1 = the build has a render-scale stub (state +0x10): the fixups table says so (RENDER_SCALE), and the iOS launcher offers the setting (dm/games/settings.tsv)
if BUILD_ID in FRAME_STEP and NAME == 'main':
    FS_STATE_OFF = KEY_PAGE_OFF + 0x4000
    assert not FAR and FS_STATE_OFF % 0x1000 == 0 and FS_STATE_OFF + 0x90 <= PATCH_OFF and not any(IMG[FS_STATE_OFF:]), 'frame step state page'
    def fs_site(off, orig):
        ob = bytes.fromhex(orig)
        assert IMG[off:off + len(ob)] == ob, f'frame step original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
        for o in range(off, off + len(ob), 4):
            assert o not in kinds and not any(seg[0].address <= o < seg[-1].address + 4 for seg in windows), f'frame step site is a rewrite site {o:#x}'
    def fs_put(off, words, why):
        old = bytes(IMG[off:off + 4 * len(words)])
        for i, w in enumerate(words): struct.pack_into('<I', IMG, off + 4 * i, w)
        cuts.append((off, old.hex(), bytes(IMG[off:off + 4 * len(words)]).hex(), why)); stat('frame_step')
if FS_STATE_OFF:   # the build's stubs (FRAME_STEP of its patches.py)
    fs = types.SimpleNamespace(here=here, emit=emit, site=fs_site, put=fs_put, state=FS_STATE_OFF, render_scale=0)
    FRAME_STEP[BUILD_ID](fs); RENDER_SCALE = fs.render_scale

# ---- Spin waits (every module) ----
# A guest loop that polls memory until another thread changes it fills only its own core on the Switch, where any higher-priority thread pinned there preempts it, so the
# spin costs the game nothing; on the host it takes a whole core from the threads doing the work. Paper Mario: The Thousand-Year Door's gfx::ParallelManager workers
# (main+0xa8e680, three, pinned at priority 45) poll their job list this way while a batch is pending; on an iPhone Rogueport ran at 24 fps with the three at 94% of a core each.
# A loop counts as a spin wait only when it provably cannot leave by itself. From its head T, every pass along the region R (the paths from T back to T, within SPIN_WIN bytes
# after T) runs only loads, register and flag arithmetic, direct branches and barriers: no store, call, SVC, system register or event wait; and no register or flag that is
# live at T is written in R (no counter, cursor or delay). One exception: an exclusive add of a constant (ld(a)xr; add/sub #k; st(l)xr; cbnz) that every path back to T
# undoes on the same address (a worker that registers with a job, sees it finished and backs out). Such a pass leaves memory and registers as it found them, so only another
# thread can end the loop, and a pause there changes only timing. R is copied into the patch area; the original back-edges jump to the copy (no pause on the first pass, so a
# single retry costs nothing), the copy's back-edges go through a spin gate, which the host takes as a yield with the yield back-off (hle_thread.c dm_spin_wait), and exits
# from the copy go back to the original code. The gate keeps every register and the flags (entry.S)
SPIN_PATHS, SPIN_STEPS = 256, 4096   # complete paths back to T; search steps (R itself lies within SPIN_WIN bytes)
SPIN_LSE = ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin')
SPIN_BAD = {'bl', 'blr', 'br', 'ret', 'eret', 'svc', 'hvc', 'smc', 'brk', 'hlt', 'udf', 'mrs', 'msr', 'sys', 'sysl', 'wfe', 'wfi', 'sev', 'sevl', 'clrex', 'dc', 'ic', 'at',
            'tlbi', 'drps', 'dcps1', 'dcps2', 'dcps3', 'blraa', 'blrab', 'blraaz', 'blrabz', 'braa', 'brab', 'braaz', 'brabz', 'retaa', 'retab', 'eretaa', 'eretab'}
spin_dec = {}
def spin_ins(a):
    if a not in spin_dec: spin_dec[a] = next(md.disasm(bytes(IMG[a:a + 4]), a), None)
    return spin_dec[a]
def spin_norm(n):
    n = {'fp': 'x29', 'lr': 'x30', 'wsp': 'sp'}.get(n, n)
    if n in ('wzr', 'xzr'): return None
    m = re.fullmatch(r'[wx](\d+)', n)
    if m: return 'x' + m.group(1)
    m = re.fullmatch(r'[bhsdqv](\d+)', n)
    return 'v' + m.group(1) if m else n
def spin_rw(ins):
    """(registers read, registers written), w / x and the vector views merged. Capstone's own lists are not enough: it lists a compare's first operand as written and the flags
    as read by cbz / tbz, and leaves out the destination of adds (sdk+0x31033c in Paper Mario: The Thousand-Year Door, adds w9,w9,#8; b.ne: a counted loop). So every register
    operand counts as well: operand 0 written and the others read, or for a load all of them written (its address registers are in capstone's reads)"""
    m = ins.mnemonic
    regs = [spin_norm(ins.reg_name(o.reg)) for o in ins.operands if o.type == ARM64_OP_REG]
    ops = set(regs) - {None}
    if m in ('cbz', 'cbnz', 'tbz', 'tbnz'): return ops, set()
    if m.startswith('b.'): return {'nzcv'}, set()
    if m == 'b': return set(), set()
    if m in ('cmp', 'cmn', 'tst', 'fcmp', 'fcmpe'): return ops, {'nzcv'}
    if m in ('ccmp', 'ccmn', 'fccmp', 'fccmpe'): return ops | {'nzcv'}, {'nzcv'}
    r, w = ins.regs_access()
    r, w = {spin_norm(ins.reg_name(x)) for x in r} - {None}, {spin_norm(ins.reg_name(x)) for x in w} - {None}
    if m.startswith(('ld', 'prfm', 'prfum')): return r, w | ops
    return r | (set(regs[1:]) - {None}), w | (set(regs[:1]) - {None})
def spin_kind(ins):
    """'br' direct branch, 'pc' adr / adrp, 'ok' a load or register / flag arithmetic, None anything else"""
    if ins is None: return None
    m = ins.mnemonic
    if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'): return 'br'
    if m in ('adr', 'adrp'): return 'pc'
    if m in SPIN_BAD or m.startswith(('st', 'cas', 'swp') + SPIN_LSE): return None
    mem = any(o.type == ARM64_OP_MEM for o in ins.operands)
    if m.startswith(('ld', 'prfm', 'prfum')): return 'ok' if mem else None   # no memory operand: a pc-relative literal load
    return None if mem else 'ok'
def spin_rmw(a):
    """ld(a)xr wA,[xN]; add|sub wB,wA,#k; st(l)xr wS,wB,[xN]; cbnz wS,<the load> at a → (xN, ±k, registers written, the four instructions), or None"""
    l = [spin_ins(a + 4 * i) for i in range(4)]
    if None in l or l[0].mnemonic not in ('ldxr', 'ldaxr') or l[1].mnemonic not in ('add', 'sub') or l[2].mnemonic not in ('stxr', 'stlxr') or l[3].mnemonic != 'cbnz': return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    if len(l[1].operands) != 3 or l[1].operands[2].type != ARM64_OP_IMM or 'lsl' in l[1].op_str: return None
    base = l[0].operands[1].mem
    if base.disp or base.index or l[2].operands[2].mem.base != base.base or l[2].operands[2].mem.disp: return None
    if r(l[1], 1) != r(l[0], 0) or r(l[2], 1) != r(l[1], 0) or r(l[3], 0) != r(l[2], 0) or l[3].operands[1].imm != a: return None
    k = l[1].operands[2].imm * (1 if l[1].mnemonic == 'add' else -1)
    return spin_norm(l[0].reg_name(base.base)), k, {spin_norm(r(l[0], 0)), spin_norm(r(l[1], 0)), spin_norm(r(l[2], 0))} - {None}, l
def spin_rebranch(ins, pc, tgt):
    """the direct branch ins re-encoded at pc with target tgt"""
    w = struct.unpack('<I', ins.bytes)[0]; off = tgt - pc
    if ins.mnemonic == 'b': return B(pc, tgt)
    if ins.mnemonic in ('tbz', 'tbnz'):
        assert -(1 << 15) <= off < (1 << 15) and off % 4 == 0, f'spin wait tbz out of range {pc:#x}'
        return (w & ~(0x3FFF << 5)) | (((off >> 2) & 0x3FFF) << 5)
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0, f'spin wait branch out of range {pc:#x}'
    return (w & ~(0x7FFFF << 5)) | (((off >> 2) & 0x7FFFF) << 5)
spin_touched = set(kinds) | done | {o for off, oid, ow, why in obs for o in (off,)} | {o for off, ob, nb, why in cuts for o in range(off, off + len(ob) // 2, 4)}
# Instruction words that end every path through them, told apart by their encoding alone: bl; br / blr / ret; svc / brk; str / strb / strh and the vector str (unsigned offset;
# unscaled, pre / post-index; register offset); stp / stnp; ldadd and the other LSE atomics. Exclusive stores are left to the full check (the exclusive add), and so are the pre /
# post-index loads: a seqlock read in SDK 15's nn::time (sdk+0x3d3270) moves a base it sets again on each pass
SPIN_STOP = ((0xFC000000, 0x94000000), (0xFE000000, 0xD6000000), (0xFF000000, 0xD4000000), (0x3FC00000, 0x39000000), (0x3FC00000, 0x3D000000), (0x3FC00000, 0x3D800000),
             (0x3FE00000, 0x38000000), (0x3FE00C00, 0x38200800), (0x3FE00000, 0x3C000000), (0x3FE00000, 0x3C800000), (0x3A400000, 0x28000000), (0x3F200C00, 0x38200000))
def spin_quick(T, hi):
    """a back-edge to T can be reached from T without passing a word of SPIN_STOP (only such heads get the full check, which decodes)"""
    seen, todo = set(), [T]
    while todo:
        a = todo.pop()
        if a in seen or not T <= a <= hi: continue
        seen.add(a); w = struct.unpack_from('<I', IMG, a)[0]; t = w >> 24
        if 0x14 <= t <= 0x17: succ = (a + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4,)
        elif t == 0x54 or t & 0x7E == 0x34: succ = (a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4, a + 4)
        elif t & 0x7E == 0x36: succ = (a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4, a + 4)
        elif any(w & m == v for m, v in SPIN_STOP): continue
        else: succ = (a + 4,)
        if T in succ: return True
        todo.extend(succ)
    return False
class SpinNo(Exception): pass
def spin_region(T, bes):
    """the spin wait region with head T (bes: the backward branches to T found by the scan) → dict, or None"""
    hi = max(bes)
    if any(s < hi + 4 and T < e for s, e in DATA_BLOCKS) or not spin_quick(T, hi): return None
    nodes, back, bad, inner = {}, set(), set(), set()
    todo = [T]
    while todo:
        a = todo.pop()
        if a in nodes or a in bad: continue
        rmw = spin_rmw(a) if a + 12 <= hi and struct.unpack_from('<I', IMG, a)[0] & 0x3FE00000 == 0x08400000 else None   # ldxr / ldaxr
        if rmw:
            n = {'kind': 'rmw', 'base': rmw[0], 'k': rmw[1], 'ins': rmw[3], 'uses': {rmw[0]}, 'defs': rmw[2], 'succ': [a + 16]}
        else:
            x = spin_ins(a); k = spin_kind(x)
            if k is None: bad.add(a); continue
            if k == 'br':
                t = x.operands[-1].imm
                succ = [t] if x.mnemonic == 'b' else [t, a + 4]
            else: succ = [a + 4]
            n = {'kind': k, 'ins': [x], 'succ': succ}
        if any(a + 4 * i in spin_touched for i in range(len(n['ins']))): bad.add(a); continue
        nodes[a] = n
        if n['kind'] == 'rmw': inner |= {a + 4, a + 8, a + 12}
        for s in n['succ']:
            if s == T: back.add(a)
            elif T < s <= hi: todo.append(s)
    if not back or inner & set(nodes) or any(s in inner for n in nodes.values() for s in n['succ']): return None
    def prune(ends):
        keep, more = set(ends), True
        while more:
            more = False
            for a, n in nodes.items():
                if a not in keep and any(s in keep and s != T for s in n['succ']): keep.add(a); more = True
        return keep
    R = prune(back)
    for a in R:
        if 'uses' not in nodes[a]: nodes[a]['uses'], nodes[a]['defs'] = spin_rw(nodes[a]['ins'][0])
    # every path from T to a back-edge: no inner cycle, and the exclusive adds on each address (a register between two writes to it) sum to 0
    bad_back, cnt = set(), [0, 0]
    def walk(a, onpath, ver, delta):
        cnt[1] += 1
        if cnt[1] > SPIN_STEPS: raise SpinNo
        n = nodes[a]
        if n['kind'] == 'rmw':
            key = (n['base'], ver.get(n['base'], 0)); delta = dict(delta); delta[key] = delta.get(key, 0) + n['k']
        if n['defs']: ver = dict(ver); ver.update({r: ver.get(r, 0) + 1 for r in n['defs']})
        for s in n['succ']:
            if s == T:
                cnt[0] += 1
                if cnt[0] > SPIN_PATHS: raise SpinNo
                if any(delta.values()): bad_back.add(a)
            elif s in R:
                if s in onpath: raise SpinNo
                walk(s, onpath | {s}, ver, delta)
    try: walk(T, {T}, {}, {})
    except SpinNo: return None
    valid = back - bad_back
    if not valid: return None
    R = prune(valid)
    # no state carried from one pass to the next: nothing live at T is written in R
    live, more = {a: set() for a in R}, True
    while more:
        more = False
        for a in sorted(R, reverse=True):
            n, out = nodes[a], set()
            for s in n['succ']:
                if s == T: out |= live[T] if a in valid else set()
                elif s in R: out |= live[s]
            new = n['uses'] | (out - n['defs'])
            if new != live[a]: live[a] = new; more = True
    if live[T] & set().union(*(nodes[a]['defs'] for a in R)): return None
    return {'T': T, 'R': sorted(R), 'nodes': nodes, 'valid': valid}
def spin_emit(g):
    T, R, nodes, valid = g['T'], g['R'], g['nodes'], g['valid']
    cpy, fixes, tramp = {}, [], {}
    for i, a in enumerate(R):
        n = nodes[a]; cpy[a] = here(); x = n['ins'][0]
        if n['kind'] == 'rmw':
            for y in n['ins'][:3]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][3], a)); emit(0)         # retry: the copy's exclusive load
        elif n['kind'] == 'pc':
            rd, tgt = num(spin_norm(x.reg_name(x.operands[0].reg))), x.operands[1].imm
            emit(ADRP(rd, here(), tgt))
            if x.mnemonic == 'adr': emit(ADDI(rd, rd, tgt & 0xFFF))
        elif n['kind'] == 'br': fixes.append((len(patch), x, None)); emit(0)
        else: emit(struct.unpack('<I', x.bytes)[0])
        if not (n['kind'] == 'br' and x.mnemonic == 'b'):
            ft = a + 4 * len(n['ins'])
            if not (i + 1 < len(R) and R[i + 1] == ft): fixes.append((len(patch), None, ft)); emit(0)   # the next instruction is not the next one copied
    pause = here()
    emit(STP_PRE(29, 30, 31, -16)); emit_gate_call(); emit(BLR(30)); ret = here(); emit(LDP_POST(29, 30, 31, 16))
    gates.append((ret, 'spin', 0, T)); emit(B(here(), cpy[T]))
    def out(t):   # an exit taken by a conditional branch: through a b next to the copy
        if t not in tramp: tramp[t] = here(); emit(B(here(), t))
        return tramp[t]
    for idx, x, info in fixes:
        pc = PATCH_OFF + idx
        if x is None: w = B(pc, cpy.get(info, info))
        elif info is not None: w = spin_rebranch(x, pc, cpy[info])
        else:
            t, cond = x.operands[-1].imm, x.mnemonic != 'b'
            d = pause if t == T and x.address in valid else cpy[t] if t in cpy and t != T else out(t) if cond else t
            w = spin_rebranch(x, pc, d)
        struct.pack_into('<I', patch, idx, w)
    for b in sorted(valid):   # the original back-edges enter the copy
        x = nodes[b]['ins'][0]
        if x.mnemonic == 'b': put_branch(b, cpy[T])
        else: st = here(); emit(spin_rebranch(x, here(), cpy[T])); emit(B(here(), b + 4)); put_branch(b, st)
    stat('spin')
spin_regions = [g for g in (spin_region(T, spin_cand[T]) for T in sorted(spin_cand)) if g]   # all found on the unchanged image first: emitting patches back-edges
spin_sites = [(g['T'], sorted(g['valid']), sum(len(g['nodes'][a]['ins']) for a in g['R'])) for g in spin_regions]
for g in spin_regions: spin_emit(g)
if os.environ.get('DM_SPIN_LIST'):
    for T, bes, n in spin_sites: print(f'{NAME} spin wait {T:#x} ({n} instructions), back-edges ' + ' '.join(f'{b:#x}' for b in bes))

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
if FAR:   # patch area before the base: file = text + ro + data (padded to 16K) + patch area
    if BIG_TEXT and len(patch) > FRONT - 0x4000 and 'DM_PATCH_FRONT' not in os.environ:
        need = (len(patch) + 0x4000 + 0xFFFF) & ~0xFFFF
        print(f'{NAME}: patch area of {len(patch):#x} bytes does not fit in {FRONT:#x} before the base; rewriting again with {need:#x}', flush=True)
        os.execve(sys.executable, [sys.executable] + sys.argv, dict(os.environ, DM_PATCH_FRONT=hex(need)))
    assert len(patch) <= FRONT - 0x4000, f'patch area of {len(patch):#x} bytes does not fit in the {FRONT:#x} before the base'
    open(P('patched.bin'), 'wb').write(IMG + bytearray(FILE_IMAGE_LEN - len(IMG)) + patch)
else:
    open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:   # the same for every module: the rewriter and every patch file, by path from dm/tools
    for src_ in (os.path.abspath(__file__), nso_code.__file__, nso_asm.__file__, *PATCH_FILES): f.write(hashlib.sha256(open(src_, 'rb').read()).hexdigest() + f'  {os.path.relpath(os.path.abspath(src_), os.path.dirname(os.path.abspath(__file__)))}\n')
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    if FAR: f.write(f'# IMAGE_SIZE\t{IMAGE_SIZE:#x}\n# FILE_IMAGE_LEN\t{FILE_IMAGE_LEN:#x}\n')   # patch area before the base (negative PATCH_OFF): whole image size, length of the image part of the file
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
    if FS_STATE_OFF: f.write(f'# AOC_TIMING_ABI\t{getattr(fs, "aoc_timing_abi", 0)}\n# FPS_STATE_OFF\t{FS_STATE_OFF:#x}\n')   # frame step state page (FRAME_STEP): the loader writes the target fps there
    if RENDER_SCALE: f.write('# RENDER_SCALE\t1\n')   # a render-scale stub reads state +0x10 (the loader ignores the key; dm/games/settings.py offers the setting by it)
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')   # the host identifies the build by it: features with hard-coded addresses only run on the build they belong to
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
    for a, tgt in tabjumps.items(): f.write(f'{a:#x}\tT\t' + ' '.join(f'{t:#x}' for t in tgt) + '\n')   # offset-table jump and all its targets
    for t, bes, n in spin_sites: f.write(f'{t:#x}\tS\t' + ' '.join(f'{b:#x}' for b in bes) + '\n')   # spin wait: its head and the back-edges that enter its copy
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{why}\n')   # Columns: offset, original bytes, new bytes, reason
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch), 'PATCH_OFF', hex(PATCH_OFF), 'slot_refs', len(fix), 'gates', len(gates))
if DATA_BLOCKS: print(NAME, 'data blocks left alone:', ', '.join(f'{s:#x}..{e:#x}' for s, e in DATA_BLOCKS))
