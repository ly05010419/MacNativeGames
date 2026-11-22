"""A minimal NSO for the rewriter's tests: one function (`mrs x0, tpidr_el0; ret`) with an eh_frame_hdr entry, MOD0,
and a .bss of the size asked for. Uncompressed segments; no game code or data."""
import struct

TEXT, RO, DATA = 0x0, 0x1000, 0x2000
FUNC = 0x100


def build(bss_size, build_id=b'\x11' * 16):
    text = bytearray(0x1000)
    struct.pack_into('<II', text, 0, 0x14000000 | (0x200 // 4), 0x10)        # b 0x200 (the start), MOD0 at 0x10
    ehs = RO + 0x100                                                           # eh_frame_hdr in rodata
    mod0 = 0x10
    struct.pack_into('<4siiiiii', text, mod0, b'MOD0', DATA - mod0, DATA + 0x100 - mod0, DATA + 0x100 + bss_size - mod0,
                     ehs - mod0, ehs + 0x20 - mod0, DATA + 0x80 - mod0)
    struct.pack_into('<II', text, FUNC, 0xD53BD040, 0xD65F03C0)               # mrs x0, tpidr_el0 ; ret
    struct.pack_into('<I', text, 0x200, 0xD65F03C0)                            # ret
    ro = bytearray(0x1000)
    o = ehs - RO
    # version 1, eh_frame_ptr udata4, fde_count udata4, table datarel sdata4
    struct.pack_into('<BBBBIIii', ro, o, 1, 0x03, 0x03, 0x3b, 0, 1, FUNC - ehs, 0)
    data = bytearray(0x1000)                                                   # dynamic table: DT_NULL
    segs = [(0x100, TEXT, len(text)), (0x100 + len(text), RO, len(ro)), (0x100 + len(text) + len(ro), DATA, len(data))]
    head = bytearray(0x100)
    head[0:4] = b'NSO0'
    for i, (fo, mo, sz) in enumerate(segs):
        struct.pack_into('<III', head, 0x10 + i * 0x10, fo, mo, sz)
    struct.pack_into('<I', head, 0x3c, bss_size)
    head[0x40:0x50] = build_id
    struct.pack_into('<III', head, 0x60, len(text), len(ro), len(data))
    return bytes(head + text + ro + data)
