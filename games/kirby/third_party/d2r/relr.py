"""Bounded ELF64 DT_RELR decoding for locally supplied NSO modules.

Encoding: https://github.com/xinuos/gabi/issues/2 (Relative Relocation Table).
Return explicit (where, implicit_addend) pairs for the existing RELATIVE loader.
"""
import struct


def decode_relr(image, tags, lower, upper):
    if not any(key in tags for key in (35, 36, 37)):
        return []
    if not all(key in tags for key in (35, 36, 37)) or tags[37] != 8 or tags[35] % 8:
        raise ValueError('Invalid ELF64 RELR dynamic tags')
    table, size = tags[36], tags[35]
    if not 0 <= table <= table + size <= len(image):
        raise ValueError('RELR table outside image')
    addresses, seen, cursor = [], set(), None

    def append(address):
        if address % 8 or not lower <= address <= upper - 8 or address + 8 > len(image):
            raise ValueError(f'RELR relocation outside writable image: {address:#x}')
        if address in seen:
            raise ValueError(f'Duplicate RELR relocation: {address:#x}')
        seen.add(address)
        addend = struct.unpack_from('<Q', image, address)[0]
        if addend >= upper:
            raise ValueError(f'RELR addend outside module: {address:#x}')
        addresses.append((address, addend))

    for offset in range(table, table + size, 8):
        entry = struct.unpack_from('<Q', image, offset)[0]
        if entry & 1 == 0:
            append(entry)
            cursor = entry + 8
        else:
            if cursor is None:
                raise ValueError('RELR bitmap before initial address')
            for bit in range(1, 64):
                if entry & (1 << bit):
                    append(cursor + (bit - 1) * 8)
            cursor += 63 * 8
    return addresses
