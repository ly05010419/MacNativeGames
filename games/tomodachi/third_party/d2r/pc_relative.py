"""Relocate PC-relative instructions without adding memory accesses to atomics."""


def relocate_adrp(word, old_pc, new_pc):
    if word & 0x9f000000 != 0x90000000:
        raise ValueError("Expected ADRP")
    immediate = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
    immediate = (immediate ^ (1 << 20)) - (1 << 20)
    target = (old_pc & ~4095) + (immediate << 12)
    delta = (target - (new_pc & ~4095)) >> 12
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError("Relocated ADRP target is out of range")
    return 0x90000000 | ((delta & 3) << 29) | (((delta >> 2) & 0x7ffff) << 5) | (word & 31)
