#!/usr/bin/env python3
"""Count the instruction sites a d2r-mobile style rewriter must handle in an NSO text segment
(see d2r-mobile dm/tools/patch_nso.py header): svc #n, mrs/msr tpidr_el0, mrs tpidrro_el0,
counter-timer reads (cntpct/cntvct/cntfrq), and any use of x18/w18 (Apple's kernel clobbers x18).
Linear sweep over text, leaving out the data blocks patch_nso.py leaves alone (nso_code.py); literal pools / jump tables inside text can add a few false positives.
Usage: insn_census.py <module.nso> [--sites <tsv out>]"""
import collections, os, struct, sys
import re
import capstone, lz4.block
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_code

X18 = re.compile(r'\b[xw]18\b')   # not '#0x18' immediates

def image_of(path):   # (text segment offset, text size, the image of all three segments)
    d = open(path, 'rb').read()
    flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
    csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return segs[0][1], segs[0][2], img

def main():
    path = sys.argv[1]; sites_out = sys.argv[sys.argv.index('--sites') + 1] if '--sites' in sys.argv else None
    base, tsz, img = image_of(path); text = bytes(img[base:base + tsz])
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    data = nso_code.data_blocks(img, base + tsz, md)
    c = collections.Counter(); svcs = collections.Counter(); sites = []
    total = 0; undecoded = 0
    skip = set()
    for s, e in data: skip.update(range(s - base, e - base, 4))
    for off in range(0, len(text) - 3, 4):
        if off in skip: continue
        r = next(md.disasm_lite(text[off:off + 4], base + off), None)
        if r is None: undecoded += 1; continue
        total += 1; addr, _, mn, ops = r; kind = None
        if mn == 'svc': kind = 'svc'; svcs[ops] += 1
        elif mn in ('mrs', 'msr'):
            for reg in ('tpidrro_el0', 'tpidr_el0', 'cntpct_el0', 'cntvct_el0', 'cntfrq_el0', 'cntpctss_el0', 'cntvctss_el0'):
                if reg in ops: kind = '%s %s' % (mn, reg); break
        if kind is None and X18.search(ops): kind = 'x18 use'
        # sub sp, sp, xN: may leave sp misaligned, which Apple's kernel faults (patch_nso.py site_sp_align)
        if kind is None and mn == 'sub' and re.fullmatch(r'sp, sp, x\d+', ops): kind = 'sub sp, sp, xN'
        if kind: c[kind] += 1; sites.append((addr, kind, mn, ops))
    print('%s: %d instructions decoded (%d undecodable words)' % (path, total, undecoded))
    if data: print('  data blocks left out: %s' % ', '.join('%#x..%#x' % b for b in data))
    for k, v in c.most_common(): print('  %-22s %d' % (k, v))
    if svcs: print('  svc numbers:', ', '.join('%s x%d' % (k, v) for k, v in sorted(svcs.items(), key=lambda kv: int(kv[0].lstrip('#'), 0))))
    if sites_out:
        with open(sites_out, 'w') as f:
            for a, k, mn, ops in sites: f.write('%#x\t%s\t%s %s\n' % (a, k, mn, ops))

if __name__ == '__main__': main()
