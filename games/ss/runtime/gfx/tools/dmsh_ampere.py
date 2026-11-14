#!/usr/bin/env python3
"""Switch 2 (NVN2) shaders: what the build reads of their control section. The code itself is Ampere machine code, which the translator decodes and emits
natively (dmsh_xlate job arch sm86: Ryujinx.Graphics.Shader's Decoders/DecoderAmpere.cs and Instructions/InstEmitAmpere*.cs).

An NVN2 control section has version 0xc9 (NVN's have version 1 and are 0x880 bytes); its length at +0x30 in BNSH files (0 elsewhere), the register count at +0x4c,
compute's thread group at +0x58. +0x38 is the length of the instructions (the 0x80-byte SPH version 4 of the graphics stages included), +0x40 that rounded up to
0x100, +0x44 the code area's: between the last two lies the shader's c1 constant segment (0x100 bytes in vertex shaders, none in pixel shaders), as NVN's
+0x700 / +0x704 delimit it."""
import struct

CTL_MAGIC, CTL_VERSION = 0x98761234, 0xc9


def is_nvn2(ctl):
    """An NVN2 control section: magic, version 0xc9"""
    return len(ctl) >= 0x48 and struct.unpack_from('<II', ctl) == (CTL_MAGIC, CTL_VERSION)


def code_len(ctl):   # the code area
    return struct.unpack_from('<I', ctl, 0x44)[0]


def inst_len(ctl):   # the instructions (SPH included)
    return struct.unpack_from('<I', ctl, 0x38)[0]


def c1_segment(ctl, code):   # the c1 constant segment at the end of the code area
    a, e = struct.unpack_from('<II', ctl, 0x40)
    return code[a:e] if a <= e <= len(code) else b''
