#!/usr/bin/env python3
"""Where a shader comes from, read from the game's own file: the stage and the key of one BNSH variation.
A shader that does not translate has no reflection, and the build's records name only its key, so this reads the RomFS itself. It shares no code with shader_split.py on purpose:
the two must agree, and the key is the same definition (sha256 of the control section, or its own shorter length, and the code area, first 16 hex digits)."""
import hashlib
import struct

STAGES = ('vs', 'hs', 'ds', 'gs', 'ps', 'cs')
NAMES = dict(vs='vertex', hs='hull', ds='domain', gs='geometry', ps='pixel', cs='compute')


def control_length(control):
    n = struct.unpack_from('<I', control, 0x20)[0]
    return n if 0x760 <= n < 0x880 else 0x880


def variation(data, base, index):
    """{key: stage name} of one variation of the BNSH at base; ValueError when the file does not hold one there."""
    if data[base:base+8] != b'BNSH\0\0\0\0':
        raise ValueError(f'no BNSH at {base:#x}')
    if data[base+0x60:base+0x64] != b'grsc':
        raise ValueError(f'BNSH at {base:#x} has no grsc block')
    count, table = struct.unpack_from('<IQ', data, base+0x7c)
    if index >= count:
        raise ValueError(f'BNSH at {base:#x} has {count} variations, not {index + 1}')
    program = struct.unpack_from('<Q', data, base+table+0x40*index+0x10)[0]
    if not program:
        raise ValueError(f'BNSH at {base:#x} variation {index} has no program')
    out = {}
    for slot, stage in enumerate(STAGES):
        at = struct.unpack_from('<Q', data, base+program+8+8*slot)[0]
        if not at:
            continue
        control_at, code_at, code_size, control_size = struct.unpack_from('<QQII', data, base+at+8)
        control = data[base+control_at:base+control_at+control_size]
        code = data[base+code_at:base+code_at+code_size]
        out[hashlib.sha256(control[:control_length(control)]+code).hexdigest()[:16]] = NAMES[stage]
    return out


def check(romfs, entry):
    """The exception's key is this variation's shader, of this stage: its origin as the game's file says, or ValueError."""
    path = romfs/entry['file']
    if not path.is_file():
        raise ValueError(f'shader origin missing: {entry["file"]}')
    found = variation(path.read_bytes(), int(entry['bnsh'], 16), entry['variation'])
    if entry['key'] not in found:
        raise ValueError(f'shader origin differs: {entry["key"]} is not in {entry["file"]} {entry["bnsh"]} variation {entry["variation"]}')
    if found[entry['key']] != entry['stage']:
        raise ValueError(f'shader stage differs: {entry["key"]} is {found[entry["key"]]} there, not {entry["stage"]}')
