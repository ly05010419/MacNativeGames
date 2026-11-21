"""Independent byte guard for the rewriter's mov-sp-from-x18 trampoline.

The pair reload uses saved old SP after SP itself changes. Recognize the whole
sequence, including slot selection and return PC; never exempt arbitrary writes.
"""
import struct


def saved_stack_switch_registers(original, block, start, return_pc, key_page, key_index):
    if original != 0x9100025f or len(block) != 40:  # mov sp, x18
        return set()
    words = struct.unpack('<10I', block)
    s, t = words[0] & 31, (words[0] >> 10) & 31
    if s == t or s in (18, 31) or t in (18, 31) or not 0 <= key_index < 512:
        return set()
    pages = (key_page - ((start + 8) & ~4095)) // 4096
    delta = return_pc - (start + 36)
    if key_page & 4095 or not -(1 << 20) <= pages < (1 << 20) or delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        return set()
    expected = (
        0xa9000000 | (126 << 15) | (t << 10) | (31 << 5) | s,  # stp s,t,[old sp,-16]
        0xd53bd060 | t,
        0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5) | s,
        0xf9400000 | (key_index << 10) | (s << 5) | s,
        0x8b000000 | (s << 16) | (t << 5) | t,
        0xf9400000 | (t << 5) | s,
        0x91000000 | (31 << 5) | t,  # retain old SP in t
        0x91000000 | (s << 5) | 31,
        0xa9400000 | (126 << 15) | (t << 10) | (t << 5) | s,
        0x14000000 | ((delta // 4) & 0x3ffffff),
    )
    return {f'x{s}', f'x{t}'} if words == expected else set()
