# Exact even-odd polygon rasteriser (sample-centre rule) and box-filtered coverage. Polygons: list of (N,2) arrays, x/y in SOURCE px.
import numpy as np

def inside(polys, Hs, Ws, scale, off=(0.0, 0.0)):
    """Bool (Hs, Ws): sample (r, c) at sample-grid centre (c+.5, r+.5) is inside the even-odd union. Sample coords = src*scale + off."""
    acc = np.zeros((Hs, Ws + 1), np.int32)
    for P in polys:
        x0 = P[:, 0] * scale + off[0]; y0 = P[:, 1] * scale + off[1]; x1 = np.roll(x0, -1); y1 = np.roll(y0, -1)
        ylo = np.minimum(y0, y1); yhi = np.maximum(y0, y1)
        r0 = np.ceil(ylo - 0.5).astype(np.int64); r1 = np.ceil(yhi - 0.5).astype(np.int64)
        r0 = np.clip(r0, 0, Hs); r1 = np.clip(r1, 0, Hs); cnt = r1 - r0; m = cnt > 0
        if not m.any(): continue
        e = np.nonzero(m)[0]; c = cnt[m]; tot = c.sum()
        idx = np.repeat(e, c); rows = np.repeat(r0[m], c) + (np.arange(tot) - np.repeat(np.cumsum(c) - c, c))
        yc = rows + 0.5; t = (yc - y0[idx]) / (y1[idx] - y0[idx]); xc = x0[idx] + t * (x1[idx] - x0[idx])
        col = np.clip(np.ceil(xc - 0.5), 0, Ws).astype(np.int64)
        np.add.at(acc, (rows, col), 1)
    return (np.cumsum(acc, axis=1)[:, :Ws] & 1).astype(bool)

def boxsum(a, w, axis):
    """Moving sum of width w (samples) along axis, centred (w even: centred between samples to the right), zero padded."""
    a = np.moveaxis(a, axis, -1); n = a.shape[-1]; lo = w // 2; hi = w - lo
    cs = np.concatenate([np.zeros(a.shape[:-1] + (1,)), np.cumsum(a, axis=-1)], axis=-1)
    i = np.arange(n); s = cs[..., np.clip(i + hi, 0, n)] - cs[..., np.clip(i - lo, 0, n)]
    return np.moveaxis(s, -1, axis)

def coverage(polys, H, W, F, ss=8, width=1.0, off=(0.0, 0.0)):
    """Coverage 0..1 on an (H*F, W*F) texel grid of src (H, W): box pre-filter of `width` texels (1 = plain box AA) centred on each texel."""
    Hs, Ws = H * F * ss, W * F * ss
    k = max(1, int(round(width * ss)))
    if k == ss:
        b = inside(polys, Hs, Ws, F * ss, (off[0] * ss, off[1] * ss)).astype(np.float32)
        return b.reshape(H * F, ss, W * F, ss).mean(axis=(1, 3))
    if (k - ss) % 2: k += 1                                # keep the box centred on the texel centre
    m = k // 2 + ss                                        # filter past the border: the traced shape continues there (the
    b = inside(polys, Hs + 2 * m, Ws + 2 * m, F * ss, (off[0] * ss + m, off[1] * ss + m)).astype(np.float32)   # game clamps to edge)
    s = (boxsum(boxsum(b, k, 0), k, 1) / (k * k))[m:m + Hs, m:m + Ws]
    return s[ss // 2::ss, ss // 2::ss][:H * F, :W * F] if ss % 2 else 0.5 * (s[ss // 2 - 1::ss, :][:, ss // 2 - 1::ss] + s[ss // 2::ss, :][:, ss // 2::ss])[:H * F, :W * F]

def screen(polys, H, W, s, phase=(0.0, 0.0), ss=8):
    """Ideal screen view: the shape at s screen px per source px, box AA over each screen pixel; texture top-left at `phase`."""
    n = int(np.ceil(W * s + phase[0])), int(np.ceil(H * s + phase[1]))
    b = inside(polys, n[1] * ss, n[0] * ss, s * ss, (phase[0] * ss, phase[1] * ss))
    return b.reshape(n[1], ss, n[0], ss).mean(axis=(1, 3))
