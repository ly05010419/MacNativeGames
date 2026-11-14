"""Outline regulariser: a traced closed outline becomes exact straight lines and circular arcs.
Corners (turn > CORNER deg within +-CW px of arc length) split the outline into runs; each run is cut greedily into the longest
pieces that a line or a circle fits within tol (max distance, source px); pieces are rebuilt exactly (lines meet at their
intersection at a corner; arcs are sampled on their circle). Straight stays straight, round stays round, wobble below tol goes."""
import numpy as np

STEP, CW, CORNER, SNAP, PIXSNAP = 0.12, 0.35, 60.0, 1.5, 0.08

def resample(P, step=STEP):
    Q = np.vstack([P, P[:1]]); d = np.hypot(*np.diff(Q, axis=0).T); s = np.r_[0, np.cumsum(d)]; L = s[-1]
    n = max(int(round(L / step)), 12); t = np.arange(n) * L / n
    return np.c_[np.interp(t, s, Q[:, 0]), np.interp(t, s, Q[:, 1])], L / n

def turning(Q, w):
    a = Q - np.roll(Q, w, 0); b = np.roll(Q, -w, 0) - Q
    return np.degrees(np.arctan2(a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0], (a * b).sum(1)))

def fit_line(X):
    c = X.mean(0); _, _, vt = np.linalg.svd(X - c, full_matrices=False)
    return ('L', c, vt[0]), float(np.abs((X - c) @ vt[1]).max())

def fit_circle(X):
    x, y = X[:, 0], X[:, 1]; A = np.c_[x, y, np.ones(len(x))]
    a, b, cc = np.linalg.lstsq(A, -(x * x + y * y), rcond=None)[0]; c = np.array([-a / 2, -b / 2]); r = np.sqrt(max(c @ c - cc, 1e-12))
    for _ in range(4):   # geometric refinement (Kasa is biased on short arcs)
        d = X - c; rho = np.maximum(np.hypot(*d.T), 1e-9); J = np.c_[-d / rho[:, None], -np.ones(len(X))]
        try: step = np.linalg.lstsq(J, -(rho - r), rcond=None)[0]
        except np.linalg.LinAlgError: break
        c = c + step[:2]; r = r + step[2]
    return ('C', c, abs(r)), float(np.abs(np.hypot(*(X - c).T) - abs(r)).max())

def segment(X, tol, rmin, rmax):
    """Greedy: longest line or circle piece from each start (line preferred unless the circle runs >= 1.25x longer)."""
    out = []; i = 0; n = len(X)
    while i < n - 1:
        bestL = bestC = None; miss = 0; j = i + 3
        while j <= n and miss < 12:
            seg = X[i:j]; L, dl = fit_line(seg); hit = False
            if dl <= tol: bestL = (j, L); hit = True
            if len(seg) >= 8:
                C, dc = fit_circle(seg)
                if dc <= tol and rmin <= C[2] <= rmax: bestC = (j, C); hit = True
            miss = 0 if hit else miss + 1; j += 1
        if bestL is None and bestC is None: out.append((i, min(i + 2, n), fit_line(X[i:min(i + 2, n)])[0])); i = min(i + 2, n) - 1; continue
        if bestC is not None and (bestL is None or bestC[0] - i > 1.25 * (bestL[0] - i)): j, g = bestC
        else: j, g = bestL
        out.append((i, j, robust(X[i:j], g))); i = j - 1
    merged = True   # greedy pieces can split one straight edge in two (the first absorbs an arc tail, tilted): merge, refit
    while merged:
        merged = False
        for k in range(len(out) - 1):
            (i0, _, g0), (_, j1, g1) = out[k], out[k + 1]
            if g0[0] != g1[0]: continue
            X2 = X[i0:j1]; g, d = fit_line(X2) if g0[0] == 'L' else fit_circle(X2)
            if d <= tol and (g[0] == 'L' or rmin <= g[2] <= rmax):
                out[k:k + 2] = [(i0, j1, robust(X2, g))]; merged = True; break
    for k in range(len(out) - 1):   # move each split to the lowest total squared error (an arc must not swallow a line's start)
        (i0, j0, g0), (i1, j1, g1) = out[k], out[k + 1]; best = None
        for t in range(max(i0 + 4, j0 - 1 - 40), min(j1 - 4, j0 - 1 + 40) + 1):
            A, B = X[i0:t + 1], X[t:j1]
            if len(A) < 4 or len(B) < 4 or (g0[0] == 'C' and len(A) < 8) or (g1[0] == 'C' and len(B) < 8): continue
            ga = (fit_line(A) if g0[0] == 'L' else fit_circle(A))[0]; gb = (fit_line(B) if g1[0] == 'L' else fit_circle(B))[0]
            sse = (dist(A, ga) ** 2).sum() + (dist(B, gb) ** 2).sum()
            if best is None or sse < best[0]: best = (sse, t, ga, gb)
        if best:
            _, t, ga, gb = best; out[k] = (i0, t + 1, robust(X[i0:t + 1], ga)); out[k + 1] = (t, j1, robust(X[t:j1], gb))
    return out

def dist(X, g):
    if g[0] == 'L': return np.abs((X - g[1]) @ np.array([-g[2][1], g[2][0]]))
    return np.abs(np.hypot(*(X - g[1]).T) - g[2])

def robust(X, g):
    """Refit a piece on its inliers: a line piece may take in the start of the next arc within tol; that tilts the plain fit."""
    for _ in range(2):
        d = np.abs((X - g[1]) @ np.array([-g[2][1], g[2][0]])) if g[0] == 'L' else np.abs(np.hypot(*(X - g[1]).T) - g[2])
        k = d <= max(0.03, 2.5 * np.median(d))
        if k.sum() < max(4, len(X) // 3): break
        g2 = (fit_line(X[k]) if g[0] == 'L' else fit_circle(X[k]))[0]
        if g[0] == 'C' and not (0.3 <= g2[2] <= 1e4): break
        g = g2
    if g[0] == 'L' and len(X) >= 3 / STEP:   # a straight edge within SNAP deg of the axes is exactly axis-aligned (UI art)
        d = g[2]; ax = np.array([1.0, 0.0]) if abs(d[0]) >= abs(d[1]) else np.array([0.0, 1.0])
        if np.degrees(np.arccos(min(1.0, abs(d @ ax)))) < SNAP:
            k = dist(X, g) <= max(0.03, 2.5 * np.median(dist(X, g))); c = X[k].mean(0) if k.sum() >= 3 else X.mean(0)
            nrm = 1 - np.argmax(ax); f = c[nrm] - round(c[nrm])   # flush on a pixel boundary (within PIXSNAP): exactly on it
            if abs(f) < PIXSNAP: c = c.copy(); c[nrm] = round(c[nrm])
            g = ('L', c, ax * np.sign(d @ ax))
    return g

def proj(g, p):
    if g[0] == 'L': c, d = g[1], g[2]; return c + d * ((p - c) @ d)
    c, r = g[1], g[2]; v = p - c; n = np.hypot(*v); return c + v / max(n, 1e-9) * r

def meet(g, h, p, lim=1.0):
    """Point where pieces g and h meet near p: their intersection (line-line, line-circle, circle-circle) nearest p; for a
    line that just misses a circle (a tangent joint) the line's point nearest the centre; else the mean projection."""
    cand = []
    if g[0] == 'L' and h[0] == 'L':
        A = np.c_[g[2], -h[2]]
        if abs(np.linalg.det(A)) > 0.05: t = np.linalg.solve(A, h[1] - g[1]); cand.append(g[1] + g[2] * t[0])
    elif g[0] != h[0]:
        L, C = (g, h) if g[0] == 'L' else (h, g); f = L[1] + L[2] * ((C[1] - L[1]) @ L[2]); dd = np.hypot(*(f - C[1]))
        if dd < C[2]: hw = np.sqrt(C[2] ** 2 - dd ** 2); cand += [f + L[2] * hw, f - L[2] * hw]
        else: cand.append(f)
    else:
        v = h[1] - g[1]; dd = np.hypot(*v)
        if 1e-9 < dd < g[2] + h[2] and dd > abs(g[2] - h[2]):
            a = (g[2] ** 2 - h[2] ** 2 + dd ** 2) / (2 * dd); hh = np.sqrt(max(g[2] ** 2 - a ** 2, 0)); m = g[1] + v * a / dd
            nrm = np.array([-v[1], v[0]]) / dd; cand += [m + nrm * hh, m - nrm * hh]
    if cand:
        q = min(cand, key=lambda c: np.hypot(*(c - p)))
        if np.hypot(*(q - p)) < lim: return q
    return (proj(g, p) + proj(h, p)) / 2

def arc_pts(g, a, b, mid):
    c, r = g[1], g[2]; t0, t1, tm = (np.arctan2(*(v - c)[::-1]) for v in (a, b, mid))
    d1 = (tm - t0) % (2 * np.pi); d2 = (t1 - t0) % (2 * np.pi); sw = d2 if d1 < d2 else d2 - 2 * np.pi
    k = max(2, int(abs(sw) * max(r, 1) / 0.15)); t = t0 + sw * np.arange(1, k) / k
    return np.c_[c[0] + r * np.cos(t), c[1] + r * np.sin(t)]

def regularise(P, tol=0.15, rmin=0.6, rmax=400.0, corner=CORNER):
    """Closed polygon P (source px) -> regularised closed polygon, and a summary string."""
    Q, ds = resample(P)
    if len(Q) < 16: return P, 'tiny'
    g, d = fit_circle(Q)   # a whole small outline (dot, hole) that is a circle stays one exact circle
    if d <= tol and 0.25 <= g[2] <= 3.0:
        t = np.linspace(0, 2 * np.pi, max(48, int(24 * g[2])), endpoint=False); return np.c_[g[1][0] + g[2] * np.cos(t), g[1][1] + g[2] * np.sin(t)], 'circle'
    w = max(1, int(round(CW / ds))); tu = turning(Q, w)
    cor = np.nonzero((np.abs(tu) > corner) & (np.abs(tu) >= np.maximum(np.roll(np.abs(tu), 1), np.roll(np.abs(tu), -1))))[0]
    keep = []                                   # one corner per cluster (within w points)
    for c in cor:
        if not keep or c - keep[-1] > w: keep.append(c)
    if len(keep) > 1 and keep[0] + len(Q) - keep[-1] <= w: keep.pop()
    hc = bool(keep); s0 = keep[0] if keep else int(np.argmax(np.abs(tu)))
    Q = np.roll(Q, -s0, 0); keep = [(k - s0) % len(Q) for k in keep] or [0]
    bounds = sorted(set(keep)) + [len(Q)]; Qc = np.vstack([Q, Q[:1]])
    pieces = []                                   # (geom, first point, last point, mid point, starts_at_corner)
    for a, b in zip(bounds[:-1], bounds[1:]):
        X = Qc[a:b + 1]
        for k, (i, j, g) in enumerate(segment(X, tol, rmin, rmax)):
            pieces.append((g, X[i], X[j - 1], X[(i + j - 1) // 2], k == 0 and hc))
    out = []; n = len(pieces)
    for k in range(n):
        g, a, b, m, _ = pieces[k]; gp = pieces[k - 1]
        s = meet(gp[0], g, a); nx = pieces[(k + 1) % n]; e = meet(g, nx[0], b)
        out.append(s)
        if g[0] == 'C': out.extend(arc_pts(g, s, e, m))
    R = np.array(out)
    nL = sum(p[0][0] == 'L' for p in pieces); nC = n - nL
    return R, f'{nL}L{nC}C{len(keep) if hc else 0}k'
