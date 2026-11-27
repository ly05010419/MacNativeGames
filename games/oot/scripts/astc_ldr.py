#!/usr/bin/env python3
"""ASTC LDR decoding for 2D blocks, written from the Khronos Data Format Specification (section "ASTC Compressed Texture
Image Formats"): block modes, integer sequence encoding (trits and quints), colour and weight unquantization, the colour
endpoint modes of the LDR profile, partition selection, weight infill, dual planes and void-extent blocks. HDR endpoint
modes, and every block the specification calls illegal, decode as the error colour (magenta), as a decoder must.

Used at packaging time (make_vr_icons.py) on the player's own icon textures; the Python standard library only. A block's
result is 16-bit per channel (what the specification interpolates); srgb=True expands the endpoints the way an sRGB
texture does (the low byte 0x80), and the top byte of each value is then the 8-bit sRGB value.
"""

ERROR = (0xffff, 0, 0xffff, 0xffff)   # magenta

# quantization levels: (range, trits, quints, bits)
LEVELS = [(2, 0, 0, 1), (3, 1, 0, 0), (4, 0, 0, 2), (5, 0, 1, 0), (6, 1, 0, 1), (8, 0, 0, 3), (10, 0, 1, 1), (12, 1, 0, 2),
          (16, 0, 0, 4), (20, 0, 1, 2), (24, 1, 0, 3), (32, 0, 0, 5), (40, 0, 1, 3), (48, 1, 0, 4), (64, 0, 0, 6),
          (80, 0, 1, 4), (96, 1, 0, 5), (128, 0, 0, 7), (160, 0, 1, 5), (192, 1, 0, 6), (256, 0, 0, 8)]


def bit(v, i):
    return (v >> i) & 1


def trit_digits(t):
    """The five trits an 8-bit trit-block value stands for (lowest first)."""
    if (t >> 2) & 7 == 7:
        c = (((t >> 5) & 7) << 2) | (t & 3)
        t4 = t3 = 2
    else:
        c = t & 0x1f
        if (t >> 5) & 3 == 3:
            t4, t3 = 2, bit(t, 7)
        else:
            t4, t3 = bit(t, 7), (t >> 5) & 3
    if c & 3 == 3:
        t2, t1, t0 = 2, bit(c, 4), (bit(c, 3) << 1) | (bit(c, 2) & (1 - bit(c, 3)))
    elif (c >> 2) & 3 == 3:
        t2, t1, t0 = 2, 2, c & 3
    else:
        t2, t1, t0 = bit(c, 4), (c >> 2) & 3, (bit(c, 1) << 1) | (bit(c, 0) & (1 - bit(c, 1)))
    return t0, t1, t2, t3, t4


def quint_digits(q):
    """The three quints a 7-bit quint-block value stands for (lowest first)."""
    if (q >> 1) & 3 == 3 and (q >> 5) & 3 == 0:
        q0 = bit(q, 0)
        return 4, 4, (q0 << 2) | ((bit(q, 4) & (1 - q0)) << 1) | (bit(q, 3) & (1 - q0))
    if (q >> 1) & 3 == 3:
        q2 = 4
        c = (((q >> 3) & 3) << 3) | ((~(q >> 5) & 3) << 1) | (q & 1)
    else:
        q2 = (q >> 5) & 3
        c = q & 0x1f
    if c & 7 == 5:
        return (c >> 3) & 3, 4, q2
    return c & 7, (c >> 3) & 3, q2


TRITS = [trit_digits(t) for t in range(256)]
QUINTS = [quint_digits(q) for q in range(128)]


def ise_bits(count, level):
    _, trits, quints, bits = LEVELS[level]
    return count * bits + (8 * count + 4) // 5 * trits + (7 * count + 2) // 3 * quints


def ise_decode(seq, count, level):
    """count values of this level from the bits of seq (an int; bits past the sequence must already be zero)."""
    _, trits, quints, n = LEVELS[level]
    mask, out, at = (1 << n) - 1, [], 0
    if trits:
        while len(out) < count:
            m, t = [], 0
            for k, tb in enumerate((2, 2, 1, 2, 1)):
                m.append((seq >> at) & mask); at += n
                t |= ((seq >> at) & ((1 << tb) - 1)) << (0, 2, 4, 5, 7)[k]; at += tb
            out += [(d << n) | low for d, low in zip(TRITS[t], m)]
    elif quints:
        while len(out) < count:
            m, q = [], 0
            for k, qb in enumerate((3, 2, 2)):
                m.append((seq >> at) & mask); at += n
                q |= ((seq >> at) & ((1 << qb) - 1)) << (0, 3, 5)[k]; at += qb
            out += [(d << n) | low for d, low in zip(QUINTS[q], m)]
    else:
        for _ in range(count):
            out.append((seq >> at) & mask); at += n
    return out[:count]


def replicate(value, bits, to):
    out, have = 0, 0
    while have < to:
        out = (out << bits) | value; have += bits
    return out >> (have - to)


def colour_unquant(level):
    """value -> 0..255 for every value of a colour level."""
    rng, trits, quints, n = LEVELS[level]
    if not trits and not quints:
        return [replicate(v, n, 8) for v in range(rng)]
    if n == 0:
        return []   # ranges 3 and 5: never a colour level (a block needs at least range 6 for its colours)
    table = []
    for v in range(rng):
        d, m = v >> n, v & ((1 << n) - 1)
        a, b, c, dd, e, f = (bit(m, i) for i in range(6))
        if trits:
            bb, cc = {1: (0, 204), 2: (b << 8 | b << 4 | b << 2 | b << 1, 93),
                      3: (c << 8 | b << 7 | c << 3 | b << 2 | c << 1 | b, 44),
                      4: (dd << 8 | c << 7 | b << 6 | dd << 2 | c << 1 | b, 22),
                      5: (e << 8 | dd << 7 | c << 6 | b << 5 | e << 1 | dd, 11),
                      6: (f << 8 | e << 7 | dd << 6 | c << 5 | b << 4 | f, 5)}[n]
        else:
            bb, cc = {1: (0, 113), 2: (b << 8 | b << 3 | b << 2, 54), 3: (c << 8 | b << 7 | c << 2 | b << 1 | c, 26),
                      4: (dd << 8 | c << 7 | b << 6 | dd << 1 | c, 13), 5: (e << 8 | dd << 7 | c << 6 | b << 5 | e, 6)}[n]
        big_a = 0x1ff * a
        t = (d * cc + bb) ^ big_a
        table.append((big_a & 0x80) | (t >> 2))
    return table


def weight_unquant(level):
    """value -> 0..64 for every value of a weight level (levels 0..11)."""
    rng, trits, quints, n = LEVELS[level]
    if rng == 3:
        return [0, 32, 64]
    if rng == 5:
        return [0, 16, 32, 48, 64]
    table = []
    for v in range(rng):
        if not trits and not quints:
            t = replicate(v, n, 6)
        else:
            d, m = v >> n, v & ((1 << n) - 1)
            a, b, c = bit(m, 0), bit(m, 1), bit(m, 2)
            bb, cc = ({1: (0, 50), 2: (b << 6 | b << 2 | b, 23), 3: (c << 6 | b << 5 | c << 1 | b, 11)}[n] if trits else
                      {1: (0, 28), 2: (b << 6 | b << 1, 13)}[n])
            big_a = 0x7f * a
            t = (big_a & 0x20) | (((d * cc + bb) ^ big_a) >> 2)
        table.append(t + 1 if t > 32 else t)
    return table


COLOUR = [colour_unquant(level) for level in range(len(LEVELS))]
WEIGHT = [weight_unquant(level) for level in range(12)]
REVERSED = bytes(int(f'{b:08b}'[::-1], 2) for b in range(256))


def block_mode(mode):
    """(weights across, weights down, dual plane, weight level) of an 11-bit block mode; None for a reserved one."""
    r, h, d, a = bit(mode, 4), bit(mode, 9), bit(mode, 10), (mode >> 5) & 3
    if mode & 3:
        r |= (mode & 3) << 1
        b, kind = (mode >> 7) & 3, (mode >> 2) & 3
        if kind == 0:
            x, y = b + 4, a + 2
        elif kind == 1:
            x, y = b + 8, a + 2
        elif kind == 2:
            x, y = a + 2, b + 8
        elif mode & 0x100:
            x, y = (b & 1) + 2, a + 2
        else:
            x, y = a + 2, (b & 1) + 6
    else:
        r |= ((mode >> 2) & 3) << 1
        if (mode >> 2) & 3 == 0:
            return None
        b, kind = (mode >> 9) & 3, (mode >> 7) & 3
        if kind == 0:
            x, y = 12, a + 2
        elif kind == 1:
            x, y = a + 2, 12
        elif kind == 2:
            x, y, d, h = a + 6, b + 6, 0, 0
        elif a == 0:
            x, y = 6, 10
        elif a == 1:
            x, y = 10, 6
        else:
            return None
    return x, y, d, (r - 2) + 6 * h


def hash52(p):
    m = 0xffffffff
    p ^= p >> 15; p = (p - (p << 17)) & m; p = (p + (p << 7)) & m; p = (p + (p << 4)) & m
    p ^= p >> 5; p = (p + (p << 16)) & m; p ^= p >> 7; p ^= p >> 3
    p = (p ^ (p << 6)) & m; p ^= p >> 17
    return p


def partition_of(seed, x, y, z, count, small):
    """The partition (0..count-1) the specification's hash gives texel (x, y, z)."""
    if small:
        x, y, z = x << 1, y << 1, z << 1
    seed += (count - 1) * 1024
    r = hash52(seed)
    s = [(r >> sh) & 0xf for sh in (0, 4, 8, 12, 16, 20, 24, 28, 18, 22, 26)] + [((r >> 30) | (r << 2)) & 0xf]
    s = [v * v for v in s]
    if seed & 1:
        sh1, sh2 = (4 if seed & 2 else 5), (6 if count == 3 else 5)
    else:
        sh1, sh2 = (6 if count == 3 else 5), (4 if seed & 2 else 5)
    sh3 = sh2 if seed & 0x10 else sh1
    shifts = (sh1, sh2, sh1, sh2, sh1, sh2, sh1, sh2, sh3, sh3, sh3, sh3)
    s = [v >> k for v, k in zip(s, shifts)]
    a = (s[0] * x + s[1] * y + s[10] * z + (r >> 14)) & 0x3f
    b = (s[2] * x + s[3] * y + s[11] * z + (r >> 10)) & 0x3f
    c = (s[4] * x + s[5] * y + s[8] * z + (r >> 6)) & 0x3f
    d = (s[6] * x + s[7] * y + s[9] * z + (r >> 2)) & 0x3f
    if count < 4:
        d = 0
    if count < 3:
        c = 0
    if a >= b and a >= c and a >= d:
        return 0
    if b >= c and b >= d:
        return 1
    return 2 if c >= d else 3


def infill(grid_x, grid_y, block_x=4, block_y=4):
    """For each texel: ((index, factor) x 4) of the weight grid, factors in sixteenths."""
    ds, dt = (1024 + block_x // 2) // (block_x - 1), (1024 + block_y // 2) // (block_y - 1)
    out = []
    for t in range(block_y):
        for s in range(block_x):
            gs, gt = (ds * s * (grid_x - 1) + 32) >> 6, (dt * t * (grid_y - 1) + 32) >> 6
            js, fs, jt, ft = gs >> 4, gs & 0xf, gt >> 4, gt & 0xf
            v0 = js + jt * grid_x
            w11 = (fs * ft + 8) >> 4
            out.append(((v0, 16 - fs - ft + w11), (v0 + 1, fs - w11), (v0 + grid_x, ft - w11), (v0 + grid_x + 1, w11)))
    return out


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else v


def transfer(a, b):
    """bit_transfer_signed: (offset a in -32..31, base b)."""
    b = (b >> 1) | (a & 0x80)
    a = (a >> 1) & 0x3f
    return (a - 0x40 if a & 0x20 else a), b


def contract(r, g, b, a):
    return (r + b) >> 1, (g + b) >> 1, b, a


def endpoints(cem, v):
    """The two RGBA endpoints (0..255) of one partition; None for an HDR mode."""
    if cem == 0:
        return (v[0],) * 3 + (255,), (v[1],) * 3 + (255,)
    if cem == 1:
        l0 = (v[0] >> 2) | (v[1] & 0xc0)
        return (l0,) * 3 + (255,), (min(l0 + (v[1] & 0x3f), 255),) * 3 + (255,)
    if cem == 4:
        return (v[0],) * 3 + (v[2],), (v[1],) * 3 + (v[3],)
    if cem == 5:
        (o1, b0), (o3, b2) = transfer(v[1], v[0]), transfer(v[3], v[2])
        return (b0,) * 3 + (b2,), tuple(map(clamp, (b0 + o1,) * 3 + (b2 + o3,)))
    if cem in (6, 10):
        alpha0, alpha1 = (v[4], v[5]) if cem == 10 else (255, 255)
        return tuple((v[i] * v[3]) >> 8 for i in range(3)) + (alpha0,), (v[0], v[1], v[2], alpha1)
    if cem in (8, 12):
        a0, a1 = (v[6], v[7]) if cem == 12 else (255, 255)
        if v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]:
            return (v[0], v[2], v[4], a0), (v[1], v[3], v[5], a1)
        return contract(v[1], v[3], v[5], a1), contract(v[0], v[2], v[4], a0)
    if cem in (9, 13):
        pairs = [transfer(v[i + 1], v[i]) for i in (0, 2, 4)] + ([transfer(v[7], v[6])] if cem == 13 else [(0, 255)])
        base = tuple(b for _, b in pairs)
        plus = tuple(b + o for o, b in pairs)
        if sum(o for o, _ in pairs[:3]) >= 0:
            return base, tuple(map(clamp, plus))
        return tuple(map(clamp, contract(*plus))), tuple(map(clamp, contract(*base)))
    return None


_MODES, _PARTS, _INFILL = {}, {}, {}


def decode_block(block, srgb=False):
    """A 16-byte ASTC 4x4 block -> 16 texels (row by row) of (r, g, b, a) in 0..65535."""
    v = int.from_bytes(block, 'little')
    mode = v & 0x7ff
    if mode & 0x1ff == 0x1fc:   # void extent: one colour
        if bit(v, 9):
            return [ERROR] * 16
        return [tuple((v >> (64 + 16 * i)) & 0xffff for i in range(4))] * 16
    if mode not in _MODES:
        _MODES[mode] = block_mode(mode)
    shape = _MODES[mode]
    if shape is None:
        return [ERROR] * 16
    gx, gy, dual, wlevel = shape
    parts = ((v >> 11) & 3) + 1
    weights_n = gx * gy * (dual + 1)
    wbits = ise_bits(weights_n, wlevel)
    if gx > 4 or gy > 4 or weights_n > 64 or not 24 <= wbits <= 96 or (dual and parts == 4):
        return [ERROR] * 16
    below = 128 - wbits
    if parts == 1:
        cems, start = [(v >> 13) & 0xf], 17
    else:
        field, start = (v >> 23) & 0x3f, 29
        if field & 3 == 0:
            cems = [field >> 2] * parts
        else:
            extra = 3 * parts - 4
            below -= extra
            bits = (field >> 2) | (((v >> below) & ((1 << extra) - 1)) << 4)
            base = (field & 3) - 1
            cems = [((base + bit(bits, i)) << 2) | ((bits >> (parts + 2 * i)) & 3) for i in range(parts)]
    if dual:
        below -= 2
        plane2 = (v >> below) & 3
    count = sum(2 * ((c >> 2) + 1) for c in cems)
    available = below - start
    if count > 18 or available <= 0:
        return [ERROR] * 16
    clevel = next((lv for lv in range(20, -1, -1) if ise_bits(count, lv) <= available), -1)
    if clevel < 4:
        return [ERROR] * 16
    values = ise_decode((v >> start) & ((1 << ise_bits(count, clevel)) - 1), count, clevel)
    values = [COLOUR[clevel][x] for x in values]
    ends = []
    for c in cems:
        n = 2 * ((c >> 2) + 1)
        pair = endpoints(c, values[:n])
        values = values[n:]
        if pair is None:
            return [ERROR] * 16
        ends.append(pair)
    reversed_block = int.from_bytes(bytes(REVERSED[b] for b in reversed(block)), 'little')
    raw = ise_decode(reversed_block & ((1 << wbits) - 1), weights_n, wlevel)
    grid = [WEIGHT[wlevel][x] for x in raw]
    if (gx, gy) not in _INFILL:
        _INFILL[gx, gy] = infill(gx, gy)
    planes = [grid[0::2], grid[1::2]] if dual else [grid]
    texel_weights = []
    for plane in planes:
        size = len(plane)
        texel_weights.append([(sum(plane[i] * f for i, f in taps if f and i < size) + 8) >> 4 for taps in _INFILL[gx, gy]])
    if parts > 1:
        seed = (v >> 13) & 0x3ff
        if (seed, parts) not in _PARTS:
            _PARTS[seed, parts] = [partition_of(seed, x, y, 0, parts, True) for y in range(4) for x in range(4)]
        which = _PARTS[seed, parts]
    else:
        which = [0] * 16
    low = 0x80 if srgb else None
    out = []
    for i in range(16):
        e0, e1 = ends[which[i]]
        texel = []
        for ch in range(4):
            w = texel_weights[1][i] if dual and ch == plane2 else texel_weights[0][i]
            lo = low if low is not None and ch < 3 else None
            c0 = (e0[ch] << 8) | (e0[ch] if lo is None else lo)
            c1 = (e1[ch] << 8) | (e1[ch] if lo is None else lo)
            texel.append((c0 * (64 - w) + c1 * w + 32) >> 6)
        out.append(tuple(texel))
    return out
