"""BLR x18 preparation without changing callee entry registers or return PC.

The preceding, movable stack access is replayed first. A borrowed register is
saved/restored in the Apple red zone, then x30 receives virtual x18. Returning
to BLR x30 at the ORIGINAL call address preserves its architectural link value.
Precondition: no alternate entry into the second word of this two-word window.
"""
import struct
import re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

BLR_X18 = 0xD63F0240
BLR_X30 = 0xD63F03C0
# register-only instructions a call window may replay (no flags read, no PC-relative form: adr/adrp/branches stay out)
REGISTER_ONLY = ('mov', 'movz', 'movk', 'movn', 'orr', 'and', 'eor', 'add', 'sub', 'lsl', 'lsr', 'asr', 'sxtw', 'uxtw',
                 'fmov', 'ubfx', 'sbfx', 'ubfiz', 'sbfiz', 'bfi', 'bfxil', 'neg', 'mvn', 'madd', 'mul')


def branch(source, target):
    delta = target - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('Call-window branch out of range')
    return 0x14000000 | ((delta >> 2) & 0x3FFFFFF)


def movable_store(word, address):
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoder.detail = True
    ins = next(decoder.disasm(struct.pack('<I', word), address), None)
    if ins is not None and ins.mnemonic in REGISTER_ONLY:
        # Mario Kart World: a register move or immediate before the call. Replayed at another PC it computes the same:
        # registers only (no SP, no memory, no PC-relative form), and neither virtual x18 nor x30, which the window uses.
        read, written = ins.regs_access()
        names = {ins.reg_name(r) for r in (*read, *written)}
        if names & {'x18', 'w18', 'x30', 'w30', 'sp', 'wsp'} or any(o.type == 3 for o in ins.operands):   # 3: memory operand
            raise ValueError(f'Call prefix must be a register-only instruction without x18, x30 or SP: {address:#x}')
        return
    if ins is None or ins.mnemonic not in ('str', 'stp', 'stur', 'ldr', 'ldp', 'ldur') or ins.writeback:
        raise ValueError(f'Call prefix must be a non-writeback stack access: {address:#x}')
    mem = ins.operands[-1].mem
    base = ins.reg_name(mem.base)
    if base not in ('sp', 'fp') or mem.index or (base == 'sp' and mem.disp < 0):
        raise ValueError(f'Call prefix must access the SP/FP stack frame: {address:#x}')
    if re.search(r'\b[xw]18\b', ins.op_str):
        raise ValueError(f'Call prefix also uses virtual x18: {address:#x}')


def prepare_call(prefix_word, prefix_address, call_address, patch_address,
                 key_page_address, key_index, scratch=0):
    if call_address != prefix_address + 4 or not 0 <= scratch < 18:
        raise ValueError('Invalid call window or scratch register')
    movable_store(prefix_word, prefix_address)
    if not 0 <= key_index < 8:
        raise ValueError('Invalid key-page index')
    adrp_pc = patch_address + 12
    pages = ((key_page_address & ~4095) - (adrp_pc & ~4095)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError('Key page out of ADRP range')
    words = [
        prefix_word,
        0xF8000000 | ((-16 & 0x1FF) << 12) | (31 << 5) | scratch,
        0xD53BD060 | 30,  # mrs x30,tpidrro_el0
        0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7FFFF) << 5) | scratch,
        0xF9400000 | (((key_page_address & 4095) // 8 + key_index) << 10) | (scratch << 5) | scratch,
        0x8B000000 | (scratch << 16) | (30 << 5) | 30,
        0xF9400000 | (30 << 5) | 30,
        0xF8400000 | ((-16 & 0x1FF) << 12) | (31 << 5) | scratch,
        branch(patch_address + 32, call_address),
    ]
    return struct.pack('<' + 'I' * len(words), *words)
