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


def branch(source, target):
    delta = target - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('Call-window branch out of range')
    return 0x14000000 | ((delta >> 2) & 0x3FFFFFF)


def movable_store(word, address):
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoder.detail = True
    ins = next(decoder.disasm(struct.pack('<I', word), address), None)
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
