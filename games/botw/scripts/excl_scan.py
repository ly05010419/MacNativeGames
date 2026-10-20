"""Count exclusive loads / stores with and without acquire / release in an analysis ELF's executable segments (base 0)."""
import struct, sys, collections
path = sys.argv[1]; b = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', b, 0x20); phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
segs = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize) for i in range(phnum)]
c = collections.Counter(); first = collections.defaultdict(list)
for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in segs:
    if p_type != 1 or not (p_flags & 1): continue
    for i in range(0, p_filesz - 3, 4):
        w, = struct.unpack_from('<I', b, p_offset + i)
        if (w & 0x3f000000) != 0x08000000: continue
        o2, L, o1, o0 = (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 15) & 1
        if o2: continue   # LDAR/STLR/LDLAR etc. (not exclusive)
        size = w >> 30
        kind = ('LD' if L else 'ST') + ('A' if (L and o0) else 'L' if (not L and o0) else '') + 'X' + ('P' if o1 else 'R') + ' size%d' % size
        c[kind] += 1
        if len(first[kind]) < 6: first[kind].append(hex(p_vaddr + i))
for k, v in sorted(c.items()): print(f'{k:16} {v:6}  e.g. {" ".join(first[k])}')
