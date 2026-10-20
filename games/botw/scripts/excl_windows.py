"""Count plain exclusive loads that start a real exclusive window (a store-exclusive with the same base register within 16
instructions) in an analysis ELF (base 0), text and everything executable."""
import struct, sys
path = sys.argv[1]; b = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', b, 0x20); phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
segs = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize) for i in range(phnum)]
def is_ldxr(w): return (w & 0x3ffffc00) == 0x085f7c00
def is_ldxp(w): return (w & 0xbfff8000) == 0x887f0000
def is_stx(w): return (w & 0x3fe07c00) == 0x08007c00 or (w & 0xbfe00000) == 0x88200000
win = miss = 0; misses = []
for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in segs:
    if p_type != 1 or not (p_flags & 1): continue
    n = p_filesz // 4; words = struct.unpack_from('<%dI' % n, b, p_offset)
    for i, w in enumerate(words):
        if not (is_ldxr(w) or is_ldxp(w)): continue
        rn = (w >> 5) & 31
        if any(is_stx(x) and ((x >> 5) & 31) == rn for x in words[i + 1:i + 17]): win += 1
        else: miss += 1; misses.append(hex(p_vaddr + 4 * i))
print('plain exclusive loads starting a window:', win, ' without a matching store-exclusive:', miss, misses[:20])
