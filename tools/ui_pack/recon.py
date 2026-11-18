# v3 shape reconstruction of UI masks: per channel and level layer, a threshold field at K x the source is refined until the
# box coverage of its level set matches the source's coverage (back-projection), then traced with ContourPy into polygons
# (source px units). fit_key() is the whole fit of one key (shapes); save/load keep the shapes as .npz.
import numpy as np
from PIL import Image
from scipy import ndimage
from scipy.spatial import cKDTree
import contourpy
from tools.ui_pack.outline import pixel_boundaries
from tools.ui_pack import param, prim, raster

def plateaus(c, minfrac=0.005):
    """Flat levels of a channel: histogram peaks of flat (3x3 range small) pixels; low/high ends snapped to the nearest peak."""
    lo, hi = float(c.min()), float(c.max()); r = hi - lo
    t = 0.04 * r; q = np.stack([c[:-1, :-1], c[1:, :-1], c[:-1, 1:], c[1:, 1:]]); blk = (q.max(0) - q.min(0)) <= t   # 2x2 flat blocks
    fl = np.zeros(c.shape, bool); fl[:-1, :-1] |= blk; fl[1:, :-1] |= blk; fl[:-1, 1:] |= blk; fl[1:, 1:] |= blk
    h = np.convolve(np.bincount(np.round(c[fl]).astype(int), minlength=256).astype(float), np.ones(5), 'same'); n = max(fl.sum(), 1)
    cand = [(i, h[i]) for i in range(256) if h[i] > 0 and h[i] == h[max(0, i - 6):i + 7].max() and h[i] >= max(12, minfrac * n)]
    snap = lambda e: max([p for p in cand if abs(p[0] - e) <= 0.1 * r] or [(e, 0)], key=lambda p: p[1])[0]
    L, H = float(snap(lo)), float(snap(hi)); mid = []
    for i, s in cand:
        if i - L > 0.1 * r and H - i > 0.1 * r:
            if mid and i - mid[-1][0] < 0.1 * r:
                if s > mid[-1][1]: mid[-1] = (i, s)
            else: mid.append((i, s))
    raw = np.bincount(np.round(c[fl]).astype(int), minlength=256)
    exact = lambda v: float(max(range(max(0, int(v) - 3), min(256, int(v) + 4)), key=lambda j: raw[j]))   # raw mode near the smoothed peak
    return [exact(L)] + [exact(m[0]) for m in mid] + [exact(H)]

def up(c, K, how=Image.BICUBIC):
    h, w = c.shape; return np.asarray(Image.fromarray(c.astype(np.float32), 'F').resize((w * K, h * K), how), dtype=np.float64)

def down(a, K):
    h, w = a.shape[0] // K, a.shape[1] // K; return a.reshape(h, K, w, K).mean(axis=(1, 3))

def soft_cover(u):
    """AA coverage at the internal grid of {u >= .5}: signed distance (internal px) from the local gradient, 1-px ramp."""
    gy, gx = np.gradient(u); g = np.maximum(np.hypot(gx, gy), 1e-4)
    return np.clip((u - 0.5) / g + 0.5, 0, 1)

SIGMAS = (0.3, 0.45, 0.6)   # floor 0.3: game AA is not an exact box filter; an exact per-pixel fit wobbles

def consistent(C, K, it=80, gain=0.5, pad=4, sig=0.0, reg=0.0, reg0=0.0, sharp=1.0, sdf=False, hard=0):
    """Threshold field (K x C) whose level set {u>=.5}, box-averaged to source size and blurred by a Gaussian of sig source px
    (the source's own edge softness; 0 = plain box anti-aliasing), matches the coverage C. Back-projection with a 1-internal-px
    ramp as the coverage model; edge-padded (the game samples clamp-to-edge). Returns u and the model error at source size."""
    Bl = (lambda a: ndimage.gaussian_filter(a, sig, mode='nearest')) if sig > 0 else (lambda a: a)
    Cp = np.pad(C, pad, mode='edge'); u = up(Cp, K)
    M = np.zeros(Cp.shape); M[pad:-pad, pad:-pad] = 1     # only real pixels constrain the fit; the padded border only lets
    for i in range(it):                                     # the field (and the trace) continue past the texture edge
        # ramp narrows over the second half (sharp x): with a 1-px ramp the fit can meet a thin tip with sub-threshold
        # "half-on" sub-pixels (u just under .5) that the binary trace then drops (4e709b0f apex, 2026-11-13)
        k = K * (1 + (sharp - 1) * max(0.0, 2 * i / it - 1))
        cv = soft_cover(u) if (sdf and i >= it // 2) else np.clip((u - 0.5) * k + 0.5, 0, 1)
        u = u + gain * up(Bl(M * (Cp - Bl(down(cv, K)))), K)
        rho = max(reg * sig, reg0)                                          # no contour detail finer than the source's blur
        if rho > 0: u = ndimage.gaussian_filter(u, rho * K)
    for i in range(hard):   # binary phase: a flat sub-threshold "fog" reads as zero coverage here, so the field must rise
        g = gain * (1 - i / max(hard, 1)) + 0.05          # until real sub-pixels turn on (tips, edges grazing a pixel column)
        u = u + g * up(Bl(M * (Cp - Bl(down((u >= 0.5).astype(float), K)))), K)
        rho = max(reg * sig, reg0)
        if rho > 0: u = ndimage.gaussian_filter(u, rho * K)
    e = (Cp - Bl(down((u >= 0.5).astype(float), K)))[pad:-pad, pad:-pad]
    return (u, pad), e

def fit_layer(C, K, it, o):
    """Pick the source blur (override 'sig', else the SIGMAS value with the lowest mean model error, fitted WITHOUT the
    smoothing term so the choice is not biased to 0), then fit with smoothing reg * sig."""
    rg = o.get('reg', 0.5)   # 0.5: smooth outlines without thinning strokes or closing holes (exp_reg2.py)
    if 'sig' in o: sg = o['sig']
    else:
        best = None
        for g in (SIGMAS if C.size <= 300 * 300 else (0.3, 0.45)):
            _, e = consistent(C, K, it, sig=g)
            m = np.abs(e).mean()
            if best is None or m < best[1] - 1e-5: best = (g, m)
        sg = best[0]
    u, e = consistent(C, K, it, sig=sg, reg=rg, reg0=o.get("reg0", 0.0), hard=o.get("hard", 0))
    return u, e, sg

def trace(u, K, tolerance=0.2, speck=None):
    """Trace the fitted level set at subpixel centres; keep border continuation and holes.

    ContourPy supplies deterministic linear contours. Shape regularisation below fits
    lines/arcs; no external curve fitter or game-specific shape data is used.
    """
    pad = 0
    if isinstance(u, tuple): u, pad = u
    if not (u >= 0.5).any(): return []
    z = np.pad(u, 1, constant_values=min(0.0, float(u.min())))
    lines = contourpy.contour_generator(x=np.arange(z.shape[1]) - 0.5,
        y=np.arange(z.shape[0]) - 0.5, z=z, name='serial', line_type='Separate').lines(0.5)
    minimum = speck if speck is not None else max(1, K * K // 2)
    result = []
    for line in lines:
        P = line[:-1] if np.array_equal(line[0], line[-1]) else line
        area = abs(np.sum(P[:, 0] * np.roll(P[:, 1], -1) - P[:, 1] * np.roll(P[:, 0], -1))) / 2
        if len(P) >= 3 and area > minimum:
            result.append(dp_closed(P, tolerance) / K - pad)
    return result

def layers(c, pl):
    """Coverage maps of the level layers (a, b): clip((c - a)/(b - a))."""
    return [(a, b, np.clip((c - a) / (b - a), 0, 1)) for a, b in zip(pl[:-1], pl[1:])]

def ncv(c, w, sig):
    """Normalised convolution: Gaussian-weighted mean of c over the pixels where w (bool)."""
    num = ndimage.gaussian_filter(c * w, sig); den = ndimage.gaussian_filter(w.astype(float), sig); return num, den

def edge_aware(c, K, it, o):
    """Hard edges traced, soft parts kept smooth: c = S + (B - S) * M. Z = pixels with a steep 4-neighbour step (> tau * range),
    dilated once. Outside Z, S = B = c. In Z, S / B are Gaussian-weighted means of the nearby dark / bright pixels outside Z
    (dark/bright split at T), or constants S0 / P when given; M = coverage-consistent trace of (c - S) / (B - S)."""
    r = np.ptp(c); tau = o.get('tau', 0.3); T = o.get('T', c.min() + r / 2)
    d = np.zeros(c.shape, bool); dx = np.abs(np.diff(c, axis=1)) > tau * r; dy = np.abs(np.diff(c, axis=0)) > tau * r
    d[:, 1:] |= dx; d[:, :-1] |= dx; d[1:, :] |= dy; d[:-1, :] |= dy
    Z = ndimage.binary_dilation(d, iterations=o.get('zdil', 1)); br = ~Z & (c > T); dk = ~Z & (c <= T)
    def field(mask, const):
        if const is not None: return np.full(c.shape, float(const))
        f = c.copy()
        for sig in (1.0, 2.0, 4.0, 8.0):
            num, den = ncv(c, mask, sig); m = Z & (den > 0.05) & (f == c) if sig == 1.0 else Z & (den > 0.01) & todo
            f[m] = num[m] / den[m]; todo = Z & ~(den > 0.05) if sig == 1.0 else todo & ~m
        return f
    S = field(dk, o.get('S0')); B = field(br, o.get('P'))
    S = np.where(Z, S, c); B = np.where(Z, B, c); gap = B - S
    cov = np.where(Z, np.clip((c - S) / np.where(np.abs(gap) > 4, gap, 1e9), 0, 1), (c > T).astype(float))
    cov = np.where(Z & (np.abs(gap) <= 4), (c > T).astype(float), cov)
    u, e, sg = fit_layer(cov, K, it, o)
    polys = trace(u, K, o.get('trace_tol', 0.2))
    if o.get('prim', 0.15): polys = regular(polys, o.get('prim', 0.15), [], o.get('corner', prim.CORNER))
    return dict(const=False, ea=True, S=S, B=B, levels=[float(c.min()), float(c.max())],
                layers=[dict(a=0.0, b=1.0, polys=polys, fit_mae=float(np.abs(e).mean()), fit_max=float(np.abs(e).max()), sig=sg)], zfrac=float(Z.mean()))

def circle_fit(P):
    """Least-squares circle (Kasa) of polygon points: centre, radius, max radial deviation, max angular gap (deg)."""
    x, y = P[:, 0], P[:, 1]; A = np.c_[x, y, np.ones(len(x))]
    a, b, c = np.linalg.lstsq(A, -(x * x + y * y), rcond=None)[0]; cx, cy = -a / 2, -b / 2; r = np.sqrt(max(cx * cx + cy * cy - c, 0))
    d = np.hypot(x - cx, y - cy); ang = np.sort(np.degrees(np.arctan2(y - cy, x - cx)))
    gap = np.max(np.diff(np.r_[ang, ang[0] + 360])) if len(ang) > 1 else 360
    return cx, cy, r, float(np.abs(d - r).max()), float(gap)

def roundify(polys, tol, log, H=None, W=None, cmin=0.8):
    """Replace outlines that are circles within tol source px by exact circles. Only the points inside the texture are judged
    (a circle may run past the border, where the padded trace is not round); they must span the circle (gap < 45 deg, or
    < 120 deg when the outline leaves the texture)."""
    out = []
    for P in polys:
        Q = P if H is None else P[(P[:, 0] >= -0.01) & (P[:, 0] <= W + 0.01) & (P[:, 1] >= -0.01) & (P[:, 1] <= H + 0.01)]
        if len(Q) >= 16:
            cx, cy, r, dev, gap = circle_fit(Q)
            if r >= cmin and dev <= tol and dev <= 0.12 * r and gap < (45 if len(Q) == len(P) else 120):
                n = int(max(96, 24 * r)); t = np.arange(n) / n * 2 * np.pi
                out.append(np.c_[cx + r * np.cos(t), cy + r * np.sin(t)]); log.append(f'circle r={r:.2f} dev={dev:.2f}'); continue
        out.append(P)
    return out

def regular(polys, tol, log, corner=prim.CORNER):
    """prim.regularise every outline except exact circles (roundify) and specks."""
    out = []
    for P in polys:
        if len(P) >= 96 and prim.fit_circle(P)[1] < 1e-6: out.append(P); continue
        R, tag = prim.regularise(P, tol, corner=corner)
        A, _ = prim.resample(P, 0.05); Bq, _ = prim.resample(R, 0.05)   # guard: a bad joint or arc sweep moves the outline
        dev = max(cKDTree(A).query(Bq)[0].max(), cKDTree(Bq).query(A)[0].max())
        if dev > 2 * tol: out.append(P); log.append(f'kept({dev:.2f})'); continue
        out.append(R); log.append(tag)
    return out

def pixel_outline(C, pad=4):
    """Exact grid edges, including holes, padded so border-touching shapes continue."""
    return [P - pad for P in pixel_boundaries(np.pad(C, pad, mode='edge') >= 0.5)]

def dp_closed(P, tol):
    """Douglas-Peucker simplification of a closed polygon (tol in the polygon's units): straight runs become one segment."""
    def dp(Q):
        if len(Q) < 3: return Q
        a, b = Q[0], Q[-1]; ab = b - a; n = np.hypot(*ab)
        d = np.abs(ab[0] * (Q[:, 1] - a[1]) - ab[1] * (Q[:, 0] - a[0])) / n if n > 1e-9 else np.hypot(*(Q - a).T)
        i = int(np.argmax(d))
        return np.vstack([dp(Q[:i + 1])[:-1], dp(Q[i:])]) if d[i] > tol else np.vstack([a, b])
    i0 = int(np.argmax(np.hypot(*(P - P.mean(0)).T))); Q = np.roll(P, -i0, axis=0); Q = np.vstack([Q, Q[:1]])
    return dp(Q)[:-1]

def reconstruct(src, K=8, it=80, opts=None):
    """src float (H, W, C). Returns dict per used channel: levels and per-layer polygons, plus fit errors."""
    opts = opts or {}; res = {}
    for i in range(src.shape[2]):
        c = src[..., i]
        if np.ptp(c) <= 24: res[i] = dict(const=True); continue
        if opts.get('mode') == 'ea' and i in opts.get('ea_ch', [0, 1]): res[i] = edge_aware(c, K, it, opts); continue
        pl = opts.get(f'levels{i}') or plateaus(c); ls = []
        for a, b, C in layers(c, pl):
            log = []
            if not ((C > 0.02) & (C < 0.98)).any() and not opts.get('nopix'):   # pixel-crisp layer: exact pixel geometry
                polys = roundify(pixel_outline(C), opts.get('ctol', 0.75), log, *C.shape); sg = -1.0
                e = C - raster.coverage(polys, *C.shape, 1, 8) if polys else C
            elif opts.get('param'):   # known glyph geometry (param.py)
                polys, e, sg, q = param.fit(C, opts['param']); log.append('param ' + ' '.join(f'{v:.3f}' for v in q))
            else:
                u, e, sg = fit_layer(C, K, it, opts)
                polys = trace(u, K, opts.get('trace_tol', 0.2), int(opts['turd'] * K * K) if 'turd' in opts else None)
                if 'poly' in opts: polys = [dp_closed(P, opts['poly']) for P in polys]   # straight-edged glyphs
                if not opts.get('noround'): polys = roundify(polys, opts.get('ctol', 0.3), log, *C.shape, cmin=opts.get('cmin', 0.8))
            if opts.get('prim', 0.15) and sg >= 0 and not opts.get('param'): polys = regular(polys, opts.get('prim', 0.15), log, opts.get('corner', prim.CORNER))   # lines + arcs
            ls.append(dict(a=a, b=b, polys=polys, fit_mae=float(np.abs(e).mean()), fit_max=float(np.abs(e).max()), sig=sg, log=log))
        res[i] = dict(const=False, levels=pl, layers=ls)
    return res

def render(res, src, F, width=1.0, ss=8):
    """Texels at F x source: per channel L0 + sum (b - a) * coverage(layer)."""
    H, W, Cn = src.shape; out = np.zeros((H * F, W * F, Cn))
    for i in range(Cn):
        r = res[i]
        if r['const']: out[..., i] = up(src[..., i], F, Image.NEAREST); continue
        if r.get('ea'):
            Su, Bu = up(r['S'], F), up(r['B'], F); out[..., i] = Su + (Bu - Su) * raster.coverage(r['layers'][0]['polys'], H, W, F, ss, width); continue
        o = np.full((H * F, W * F), float(r['levels'][0]))
        for l in r['layers']: o += (l['b'] - l['a']) * raster.coverage(l['polys'], H, W, F, ss, width)
        out[..., i] = o
    return out

def fit_key(src, o):
    """The shapes of one key (the per-key logic of v3's recon.py CLI): src float (H, W, C), o the key's options. K is 8 (4 above 256
    px), it 80. 'sym' fits the source averaged with its mirror; 'pix' boxes are hidden from the trace (the median of the box
    border) and pasted as source pixels by pack.render; big sources get no lines+arcs unless 'prim' says so."""
    k = o.get('K', 8 if max(src.shape[:2]) <= 256 else 4)
    if 'sym' in o:
        if 'y' in o['sym']: src = (src + src[::-1]) / 2
        if 'x' in o['sym']: src = (src + src[:, ::-1]) / 2
    for y0, y1, x0, x1, i in o.get('pix', []):
        ring = np.r_[src[y0, x0:x1, i], src[y1 - 1, x0:x1, i], src[y0:y1, x0, i], src[y0:y1, x1 - 1, i]]; src = src.copy(); src[y0:y1, x0:x1, i] = np.median(ring)
    if max(src.shape[:2]) > 256: o = {'prim': 0, **o}
    return reconstruct(src, k, o.get('it', 80), o)
