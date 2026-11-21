#!/usr/bin/env python3
"""Fuzz the in-memory shader container readers of dm/gfx/tools/shader_split.py, dispatched as scan_file does for a loose file: scan_blob (BNSH, BAHS, MSHD, SARC and Yaz0
around them, and the others it recognizes by magic), then glslc_shaders when nothing was found, sdf_shaders for 'SDF\\0' (RE Engine) and arh_entries for 'arh1' (Monolith's
index), and uniq_keys / var_pairs over what they return.
Seeds: one synthetic shader pair (a 0x880-byte control section and a small code area per stage, vertex and pixel), wrapped as a BNSH, a BAHS with its binary and program arrays,
an MSHD, a SARC of the BNSH compressed with Yaz0, a single-stage and a two-stage GLSLC output, an SDF definition, and an ARH1 index with one path.
Clean rejection: red() (SystemExit with a '[RED] ' message naming the problem), or a result (an empty one included). Any other exception is a finding: scan_file's net still
turns it into an 'unreadable' red line, but one that names no layout problem (test_split_unreadable.py: a reader names its layout, "not an IndexError"); a hang or an oversized
allocation gets past that net. Usage: see fuzzlib.py"""
import struct
import sys

import fuzzlib

ss = fuzzlib.import_shader_split()

HARNESS = 'shader_split'
fuzzlib.guard(ss)
CTL, GFX, GLSLC = ss.CTL_MAGIC, ss.GFX_MAGIC, b'\x91\x68\x86\x19'


def stage(sph_type):
    """(control section, code area) of one stage: the control's +0x20 holds its length, +0x704 the code area's; the code area is a 0x30 header, the SPH (word 0: stage in bits 10..13),
    then a few instruction words"""
    code = bytearray(GFX + bytes(0x2c)); code += struct.pack('<I', sph_type) + bytes(0x4c); code += bytes.fromhex('0f00000000000000') * 8
    ctl = bytearray(0x880); ctl[:4] = CTL; struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), bytes(code)


VS, PS = stage(0x0461), stage(0x1462)


def bnsh():
    d = bytearray(0x300); d[:8] = b'BNSH\0\0\0\0'; d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)              # one variation, its table at 0x100
    struct.pack_into('<Q', d, 0x110, 0x140)                 # variation 0: the binary program at 0x140
    for si, desc, (ctl, code) in ((0, 0x200, VS), (4, 0x240, PS)):
        struct.pack_into('<Q', d, 0x148 + 8 * si, desc)     # stage slots from program +8: vertex 0, pixel 4
        at = len(d); d += ctl + code
        struct.pack_into('<QQII', d, desc + 8, at, at + len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x1c, len(d))
    return bytes(d)


def bahs():
    d = bytearray(0x100); d[:4] = b'BAHS'
    struct.pack_into('<II', d, 0x40, 8 + 2 * 0x20, 2)       # the binary array: two records of 0x20 bytes, each holding the 16-byte entry at +0x10
    region = 0x100 + 2 * 0x880
    for i, (ctl, code) in enumerate((VS, PS)):
        rec = 0x48 + 0x20 * i; ent = rec + 0x10; c = 0x100 + 0x880 * i
        struct.pack_into('<II', d, rec, 0x20, i)            # record size, kind (0 vertex, 1 fragment)
        struct.pack_into('<IIIi', d, ent, 0x100 * i, 0, len(ctl), c - ent)
    struct.pack_into('<II', d, 0x88, 8 + 16, 1); struct.pack_into('<IIII', d, 0x90, 16, 0, 0x11, 0)   # the program array: one program from binary 0
    d += VS[0] + PS[0]
    code = bytearray(0x200); code[:len(VS[1])] = VS[1]; code[0x100:0x100 + len(PS[1])] = PS[1]
    struct.pack_into('<II', d, 0x18, len(code), region); d += code
    return bytes(d)


def mshd(): return b'MSHD' + bytes(12) + VS[0] + VS[1] + PS[0] + PS[1]


def yaz0(b):   # every byte a literal: a group header 0xff per 8 bytes
    out = bytearray(b'Yaz0' + len(b).to_bytes(4, 'big') + bytes(8))
    for i in range(0, len(b), 8): out += b'\xff' + b[i:i + 8]
    return bytes(out)


def sarc(members):
    names = b''.join(n + b'\0' + bytes(-(len(n) + 1) % 4) for n, _ in members)
    sfat = b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65)
    data, at, offs = b'', 0, []
    for n, m in members:
        offs.append(at); data += m + bytes(-len(m) % 0x100); at = len(data)
    noff = 0
    for (n, m), o in zip(members, offs):
        sfat += struct.pack('<IIII', 0, 0x01000000 | noff // 4, o, o + len(m)); noff += len(n) + 1 + (-(len(n) + 1) % 4)
    head = 0x14 + len(sfat) + 8 + len(names); doff = (head + 0xff) & ~0xff
    d = b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, doff + len(data), doff) + bytes(4) + sfat + b'SFNT' + struct.pack('<HH', 8, 0) + names
    return d + bytes(doff - len(d)) + data


def glslc(stages):
    """A GLSLC output: magic, +0x44 the whole length, +0x4c the section count, section headers at +0x90 + 0x90 i ({size, data offset, type 0 = GPU code}, +0x30 control offset,
    +0x34 code offset, +0x38 code length, +0x3c control length); each section's data is its control section with the code area right after it"""
    d = bytearray(0x90 * (len(stages) + 1)); d[:4] = GLSLC; struct.pack_into('<I', d, 0x4c, len(stages))
    for i, (ctl, code) in enumerate(stages):
        at = len(d); d += ctl + code
        h = 0x90 + 0x90 * i; struct.pack_into('<III', d, h, len(ctl) + len(code), at, 0); struct.pack_into('<4I', d, h + 0x30, 0, len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x44, len(d))
    return bytes(d)


def sdf():
    d = bytearray(0x10 + 0x108); d[:4] = b'SDF\0'; struct.pack_into('<HH', d, 4, 1, 1)
    for slot, (ctl, code) in ((0, VS), (4, PS)):
        struct.pack_into('<Q', d, 0x10 + 0x10 + 8 * slot, len(d)); d += struct.pack('<II', len(ctl), len(code)) + ctl + code
    return bytes(d)


def arh1():
    """One path, 'arch.bin': node 0 the root (next 0), node 0x61 ('a' ^ root next) a leaf whose next is minus its suffix's offset in the strings ('rch.bin', then the file ID)"""
    strings = b'\0rch.bin\0' + struct.pack('<I', 7); strings += bytes(-len(strings) % 4)
    nodes = [(0, 0)] * 0x62; nodes[0x61] = (-1, 0)
    nodes_b = b''.join(struct.pack('<ii', *n) for n in nodes); files = struct.pack('<Q4I', 0x40, 0x20, 0x20, 0, 7)
    key = 0x12345678; word = (key ^ 0xf3f35353).to_bytes(4, 'little')
    enc = lambda b: bytes(v ^ word[i & 3] for i, v in enumerate(b))
    so = 0x30; no = so + len(strings); fo = no + len(nodes_b)
    return b'arh1' + struct.pack('<9I', 0, len(nodes), so, len(strings), no, len(nodes_b), fo, 1, key) + bytes(0x30 - 40) + enc(strings) + enc(nodes_b) + files


def seeds():
    b = bnsh()
    return {'bnsh': b, 'bahs': bahs(), 'mshd': mshd(), 'sarc-yaz0': yaz0(sarc([(b'shader.bnsh', b)])), 'glslc': glslc([VS]), 'glslc-program': glslc([VS, PS]),
            'sdf': sdf(), 'arh1': arh1()}


def check(data):
    try:
        if data[:4] == b'SDF\0': found = ss.sdf_shaders(data, '/fuzz.sdf')
        elif data[:4] == b'arh1': ss.arh_entries(data); return
        else:
            found = []; ss.scan_blob(data, '/fuzz', found)
            if not found and data[:4] != b'MSHD' and GLSLC in data: found += ss.glslc_shaders(data, '/fuzz', need=False)
        ss.uniq_keys(found); ss.var_pairs(found)
    except SystemExit as e:
        if not str(e).startswith('[RED] '): raise   # red(): the declared rejection
    finally: ss.BAHS_VAR.clear()


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
