#!/usr/bin/env python3
"""NSO -> ELF64 (AArch64, ET_DYN) so Ghidra's stock ELF loader can analyze a Switch module.
Usage: nso2elf.py <module.nso> <out.elf>
Keeps the NSO memory layout (vaddr = offset in the loaded image): text R-X, rodata R, data+bss RW,
plus PT_DYNAMIC (located through MOD0) so dynsym imports/exports and RELA relocations are recovered,
and PT_GNU_EH_FRAME (eh_frame_hdr from MOD0) as a function-start hint. No section headers."""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso

PAGE = 0x1000; HDR = 0x1000                     # file offset of image byte 0; keeps p_offset == p_vaddr (mod PAGE)

def load(path):
    m = nso.cli_read(path)
    return bytes(m.image), list(m.segments), m.bss, m.build_id

def main():
    src, dst = sys.argv[1:3]
    img, (text, ro, data), bss, build_id = load(src)
    mod0 = struct.unpack_from('<I', img, 4)[0]
    if img[mod0:mod0 + 4] != b'MOD0': sys.exit('no MOD0 header')
    dyn_off, bss_s, bss_e, eh_s, eh_e = (mod0 + x for x in struct.unpack_from('<iiiii', img, mod0 + 4))
    n = 0
    while struct.unpack_from('<q', img, dyn_off + n * 16)[0] != 0: n += 1
    dyn_sz = (n + 1) * 16
    R, W, X = 4, 2, 1
    ph = [(1, R | X, text[0], text[1], text[1]),                                   # PT_LOAD text
          (1, R, ro[0], ro[1], ro[1]),                                             # PT_LOAD rodata
          (1, R | W, data[0], data[1], max(data[1], bss_e - data[0], data[1] + bss)),  # PT_LOAD data + bss
          (2, R | W, dyn_off, dyn_sz, dyn_sz)]                                     # PT_DYNAMIC
    if eh_e > eh_s: ph.append((0x6474e550, R, eh_s, eh_e - eh_s, eh_e - eh_s))     # PT_GNU_EH_FRAME
    ehdr = struct.pack('<4sBBBBB7sHHIQQQIHHHHHH', b'\x7fELF', 2, 1, 1, 0, 0, b'\0' * 7,
                       3, 183, 1, text[0], 0x40, 0, 0, 0x40, 0x38, len(ph), 0x40, 0, 0)   # ET_DYN, EM_AARCH64
    phdrs = b''.join(struct.pack('<IIQQQQQQ', t, f, HDR + va, va, va, fsz, msz, PAGE if t == 1 else 8)
                     for t, f, va, fsz, msz in ph)
    out = bytearray(HDR + len(img)); out[0:len(ehdr)] = ehdr; out[0x40:0x40 + len(phdrs)] = phdrs
    out[HDR:] = img
    open(dst, 'wb').write(out)
    print('%s: text %#x+%#x ro %#x+%#x data %#x+%#x bss %#x dyn %d entries eh_frame_hdr %s build_id %s'
          % (dst, *text, *ro, *data, bss, n, '%#x' % eh_s if eh_e > eh_s else 'none', build_id.hex()))

if __name__ == '__main__': main()
