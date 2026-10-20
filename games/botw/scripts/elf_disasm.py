"""Disassemble an analysis ELF range. usage: elf_disasm.py <elf> <start> <end> [base: 0 for Breath of the Wild analysis ELFs]"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
path, s, e = sys.argv[1], int(sys.argv[2], 0), int(sys.argv[3], 0)
b = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', b, 0x20); phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
segs = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize) for i in range(phnum)]
top = max(x[3] + x[6] for x in segs); raw = bytearray(top)
for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in segs:
    if p_type == 1: raw[p_vaddr:p_vaddr + p_filesz] = b[p_offset:p_offset + p_filesz]
base = 0x100000 if len(sys.argv) < 5 else int(sys.argv[4], 0)
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
for i in md.disasm(bytes(raw[base + s: base + e]), s): print(f'{i.address:#x}: {i.mnemonic} {i.op_str}')
