"""Histogram of ASTC weight-grid sizes in a raw block dump; a grid wider or taller than the block footprint is an error
block (magenta), so the footprints every block fits tell which ASTC format the data really is. usage: astc_grids.py <file>"""
import collections, sys
def grid(bm):
    if bm & 0x1ff == 0x1fc: return 'void'
    a = (bm >> 5) & 3
    if bm & 3:
        b = (bm >> 7) & 3; lay = (bm >> 2) & 3
        if lay == 0: return (b + 4, a + 2)
        if lay == 1: return (b + 8, a + 2)
        if lay == 2: return (a + 2, b + 8)
        b &= 1; return (b + 2, a + 2) if bm & 0x100 else (a + 2, b + 6)
    if (bm >> 2) & 3 == 0: return 'reserved'
    b = (bm >> 9) & 3; lay = (bm >> 7) & 3
    if lay == 0: return (12, a + 2)
    if lay == 1: return (a + 2, 12)
    if lay == 2: return (a + 6, b + 6)
    return {0: (6, 10), 1: (10, 6)}.get(a, 'reserved')
data = open(sys.argv[1], 'rb').read(); n = len(data) // 16
hist = collections.Counter(grid(int.from_bytes(data[16 * i:16 * i + 2], 'little') & 0x7ff) for i in range(n))
print(n, 'blocks; most common grids:', hist.most_common(8))
for fw, fh in [(4, 4), (5, 4), (5, 5), (6, 5), (6, 6), (8, 5), (8, 6), (8, 8), (10, 5), (10, 6), (10, 8), (10, 10), (12, 10), (12, 12)]:
    ok = sum(c for g, c in hist.items() if g == 'void' or (isinstance(g, tuple) and g[0] <= fw and g[1] <= fh))
    print(f'{fw}x{fh}: {100 * ok / n:.1f}% of blocks valid')
