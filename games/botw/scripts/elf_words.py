"""Print the 32-bit words at module offsets of an analysis ELF (base 0). usage: words.py <elf> <off> ..."""
import struct, sys
path = sys.argv[1]; b = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', b, 0x20); phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
segs = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize) for i in range(phnum)]
for o in sys.argv[2:]:
    off = int(o, 0)
    for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in segs:
        if p_type == 1 and p_vaddr <= off < p_vaddr + p_filesz:
            print(f'{off:#x}: {struct.unpack_from("<I", b, p_offset + off - p_vaddr)[0]:#010x}')
