"""Find test-and-test-and-set spin lock acquisitions (ldr; cmp #1; b.eq; ld{a}xr; cbnz; st{l}xr; cbnz) and count the ones
taken with a plain ldxr (no acquire) versus ldaxr; also whether a dmb follows the acquisition within 4 instructions."""
import struct, sys, collections
path = sys.argv[1]; b = open(path, 'rb').read()
phoff, = struct.unpack_from('<Q', b, 0x20); phentsize, phnum = struct.unpack_from('<HH', b, 0x36)
segs = [struct.unpack_from('<IIQQQQQQ', b, phoff + i * phentsize) for i in range(phnum)]
c = collections.Counter(); ex = collections.defaultdict(list)
for p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align in segs:
    if p_type != 1 or not (p_flags & 1): continue
    n = p_filesz // 4; w = struct.unpack_from('<%dI' % n, b, p_offset)
    for i in range(n - 4):
        x = w[i]
        if (x & 0x3ffffc00) not in (0x085f7c00, 0x085ffc00): continue          # ld{a}xr
        acq = bool(x & 0x8000); rt = x & 31; rn = (x >> 5) & 31
        y = w[i + 1]
        if (y & 0x7f000000) != 0x35000000 or (y & 31) != rt: continue           # cbnz rt
        z = w[i + 2]
        if (z & 0x3fe07c00) != 0x08007c00 or ((z >> 5) & 31) != rn: continue    # st{l}xr on same base
        fence = any((v & 0xfffff0ff) == 0xd50330bf for v in w[i + 4:i + 8])      # dmb within the next few
        k = ('ldaxr' if acq else 'ldxr') + (' +dmb' if fence else '')
        c[k] += 1
        if len(ex[k]) < 5: ex[k].append(hex(p_vaddr + 4 * i))
for k, v in sorted(c.items()): print(f'{k:12} {v:6}  e.g. {" ".join(ex[k])}')
