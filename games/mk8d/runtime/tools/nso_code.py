#!/usr/bin/env python3
"""Where an NSO's text segment holds code: function ranges from .eh_frame, and data blocks in the text that no function covers.
patch_nso.py and insn_census.py import it. Usage on its own: nso_code.py <module.nso | decompressed image .bin> (lists the data blocks)

A data block is a stretch of at least DATA_MIN bytes between FDE ranges in which more than a quarter of the words do not decode as instructions. Absolum's main
(.NET NativeAOT on SDK 18) starts its text with 1.7 MB of data that no code references (no adrp, no relocation): 62% of its words do not decode, and 6,858 of the
rest read as instructions that use x18, which a linear sweep would rewrite. Code without unwind information (zlib, libunwind's register restore) stays code: it decodes."""
import struct

DATA_MIN = 0x1000


def eh_frame_hdr(img):
    """(eh_frame_hdr offset, its end) from the MOD0 header"""
    m0 = struct.unpack_from('<I', img, 4)[0]
    assert img[m0:m0 + 4] == b'MOD0', 'no MOD0'
    return m0 + struct.unpack_from('<i', img, m0 + 16)[0], m0 + struct.unpack_from('<i', img, m0 + 20)[0]


def _read(img, o, enc, ehs):
    fmt, app = enc & 0xF, enc & 0x70
    if fmt == 0x3: v, sz = struct.unpack_from('<I', img, o)[0], 4
    elif fmt == 0xB: v, sz = struct.unpack_from('<i', img, o)[0], 4
    elif fmt in (0x0, 0x4): v, sz = struct.unpack_from('<Q', img, o)[0], 8
    elif fmt == 0xC: v, sz = struct.unpack_from('<q', img, o)[0], 8
    else: raise ValueError(f'pointer encoding {enc:#x} at {o:#x}')
    if app == 0x10: v += o
    elif app == 0x30: v += ehs
    return v, sz


def _uleb(img, o):
    r = sh = 0
    while True:
        b = img[o]; o += 1; r |= (b & 0x7F) << sh; sh += 7
        if not b & 0x80: return r, o


def fde_table(img):
    """[(function start, FDE offset)] from the eh_frame_hdr binary search table; empty when the module has no valid eh_frame_hdr (New Super Mario Bros. U Deluxe's MOD0 points elsewhere)"""
    ehs, ehe = eh_frame_hdr(img)
    if ehe - ehs < 12 or img[ehs] != 1 or img[ehs + 3] in (0xFF, 0): return []
    _, penc, cenc, tenc = img[ehs:ehs + 4]
    o = ehs + 4
    _, s = _read(img, o, penc, ehs); o += s
    n, s = _read(img, o, cenc, ehs); o += s
    out = []
    for _ in range(n):
        a, s = _read(img, o, tenc, ehs); o += s
        f, s = _read(img, o, tenc, ehs); o += s
        out.append((a, f))
    return out


def fde_ranges(img):
    """Sorted [(start, end)] of every function the FDEs describe (pc begin and pc range, in the encoding the FDE's CIE gives)"""
    ehs, _ = eh_frame_hdr(img)
    cies = {}

    def cie_encoding(cie):
        if cie not in cies:
            p = cie + 8; ver = img[p]; p += 1
            aug = img[p:img.index(b'\0', p)]; p += len(aug) + 1
            _, p = _uleb(img, p)                                  # code alignment
            while img[p] & 0x80: p += 1                           # data alignment (sleb)
            p += 1
            p = p + 1 if ver == 1 else _uleb(img, p)[1]           # return address register
            enc = 0
            if aug.startswith(b'z'):
                _, p = _uleb(img, p)
                for ch in aug[1:]:
                    if ch == ord('R'): enc = img[p]; p += 1
                    elif ch == ord('P'): pe = img[p]; p += 1; p += _read(img, p, pe, ehs)[1]
                    elif ch == ord('L'): p += 1
            cies[cie] = enc
        return cies[cie]

    out = []
    for _, f in fde_table(img):
        cptr = f + 4
        enc = cie_encoding(cptr - struct.unpack_from('<I', img, cptr)[0])
        st, s = _read(img, f + 8, enc, ehs)
        ln, _ = _read(img, f + 8 + s, enc & 0xF, ehs)
        out.append((st, st + ln))
    return sorted(out)


def data_blocks(img, text_end, md):
    """Sorted [(start, end)] of the data blocks in text [0, text_end) (see the module docstring). md: a capstone AArch64 disassembler"""
    # An NRO's text includes its header: the build ID at +0x40 is arbitrary bytes, not instructions. FFX/X-2's magic_0898_2015.nro has a literal ldr x18 there.
    # Keep the entry branch and MOD0 pointer (+0..+8), but never scan the reserved word or NRO0 header (+8..+0x80), even when this gap is shorter than DATA_MIN.
    nro = img[0x10:0x14] == b'NRO0'
    gaps, last = [], 0x80 if nro else 0
    for s, e in fde_ranges(img):
        if s >= text_end: break
        if s > last: gaps.append((last, s))
        last = max(last, e)
    if last < text_end: gaps.append((last, text_end))
    out = [(8, 0x80)] if nro else []
    for s, e in gaps:
        s, e = (s + 3) & ~3, e & ~3
        if e - s < DATA_MIN: continue
        undecodable = sum(1 for a in range(s, e, 4) if next(md.disasm_lite(bytes(img[a:a + 4]), a), None) is None)
        if undecodable * 4 > (e - s) // 4: out.append((s, e))
    return out


if __name__ == '__main__':
    import sys
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    p = sys.argv[1]; raw = open(p, 'rb').read()
    if raw[:4] == b'NSO0':
        import nso
        try: m = nso.load(raw, p)
        except nso.Error as e: sys.exit(f'[RED] {e}')
        img, text_end = m.image, m.text_end
    else:
        img = raw; text_end = int(sys.argv[2], 0)
    blocks = data_blocks(img, text_end, Cs(CS_ARCH_ARM64, CS_MODE_ARM))
    print(f'{p}: {len(blocks)} data blocks in text, {sum(e - s for s, e in blocks)} bytes')
    for s, e in blocks: print(f'  {s:#x}..{e:#x} ({e - s} bytes)')


def out_store(img, start, size, md):
    """(offset, register) of the one store through a function's second argument, an out pointer (x1, copied once in the prologue: mov xB, x1 ... str xA, [xB]):
    where the SDK's VammManager::AllocateAddressRegion hands the new region's address back (patch_nso.py "Address regions"). ValueError when that shape is not there"""
    import re
    ins = list(md.disasm(bytes(img[start:start + size]), start))
    out = [i for i in ins[:16] if i.mnemonic == 'mov' and re.fullmatch(r'x\d+, x1', i.op_str)]
    if len(out) != 1: raise ValueError('no single copy of the out pointer in the prologue')
    xb = out[0].op_str.split(',')[0]
    st = [i for i in ins if i.mnemonic == 'str' and re.fullmatch(rf'x\d+, \[{xb}\]', i.op_str)]
    if len(st) != 1: raise ValueError(f'{len(st)} stores through the out pointer')
    return st[0].address, int(st[0].op_str.split(',')[0][1:])
