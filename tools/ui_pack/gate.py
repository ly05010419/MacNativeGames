"""The gate of one key (v3's build3.py gate and ink.py): a failing key is not written.
Checks (names as in a key's `accept` option):
  ink     the game view (bilinear at s_d, phase (.37, .61)) of the pack has the source's mean level, within INK per used channel
  levels  the low and high plateau values (ends) of the pack are within LEVELS of the source's
  rt      the exact render, area-downscaled to the source (through the fit's blur), differs from the source by at most RT mean levels
          in the edge band (texels within 2 of a plateau step)
  gv      the game view of the pack is at most GV (RMSE levels) farther from the ideal (the shape box-AA'd at screen px) than the
          game view of the original is
Reported, not failing: bias (signed coverage per edge crossing, source px, + = grows), topology (8-connected parts / holes per channel)."""
import numpy as np
from scipy import ndimage

from tools.ui_pack import gameview, raster
from tools.ui_pack.pack import used_channels

INK, LEVELS, RT, GV = 1.0, 2, 10.0, 0.5
INK_PHASE = (0.37, 0.61)
PHASES = [(0.0, 0.0), (0.37, 0.61), (0.5, 0.25), (0.81, 0.13)]


def ends(c):
    """Low and high plateau value: the most common value among the darkest / brightest texels (within 10% of the range)."""
    v = np.round(c).astype(int).ravel(); lo, hi = v.min(), v.max(); r = max(hi - lo, 1)
    def mode(sel): return int(np.bincount(v[sel]).argmax())
    return mode(v <= lo + 0.1 * r), mode(v >= hi - 0.1 * r)


def topo(a, ch):
    p = h = 0
    for i in ch:
        c = a[..., i]; b = c > (c.min() + np.ptp(c) / 2); p += ndimage.label(b, np.ones((3, 3)))[1]
        h += ndimage.label(np.pad(~b, 1, constant_values=True))[1] - 1
    return p, h


def ideal(res, src, s, ph, ss=8):
    H, W, Cn = src.shape; n = int(np.ceil(W * s + ph[0])), int(np.ceil(H * s + ph[1])); o = np.zeros((n[1], n[0], Cn))
    for i in range(Cn):
        r = res[i]
        if r['const']: o[..., i] = src[0, 0, i]; continue
        if r.get('ea'):
            Sv, Bv = gameview.view(r['S'], 1, s, ph), gameview.view(r['B'], 1, s, ph)
            o[..., i] = Sv + (Bv - Sv) * raster.screen(r['layers'][0]['polys'], H, W, s, ph, ss); continue
        q = np.full((n[1], n[0]), float(r['levels'][0]))
        for l in r['layers']: q += (l['b'] - l['a']) * raster.screen(l['polys'], H, W, s, ph, ss)
        o[..., i] = q
    return o


def gv_rmse(t, F, res, src, s, ch):
    """RMSE of the game view of texture t (F x source) against the ideal, mean over the phases (one phase for a big source)."""
    e = []
    big = max(src.shape[:2]) > 256
    for ph in (PHASES[1:2] if big else PHASES):
        d = gameview.view(t, F, s, ph) - ideal(res, src, s, ph, 4 if big else 8); e.append(np.sqrt(np.mean(d[..., ch] ** 2)))
    return float(np.mean(e))


def measure(src, res, img, dn, F, sd):
    """The gate values of a pack picture img (uint8 (H*F, W*F, C)) of src (float (H, W, C)); dn from pack.render."""
    H, W, Cn = src.shape; ch = used_channels(src); tex = img.astype(float)
    near = np.zeros((H, W), bool); bias = n = 0
    for i in ch:
        b = src[..., i] > src[..., i].min() + np.ptp(src[..., i]) / 2; near |= ndimage.binary_dilation(b ^ ndimage.binary_erosion(b), iterations=2)
        n += (b[:, 1:] != b[:, :-1]).sum() + (b[1:, :] != b[:-1, :]).sum()
        bias += (dn[..., i] - src[..., i]).sum() / max(res[i]['levels'][-1] - res[i]['levels'][0], 1)
    err = np.abs(dn - src)[..., ch][near] if near.any() else np.zeros(1)
    ink, lev = [], []
    for i in ch:
        ink.append(float(gameview.view(tex[..., i], F, sd, INK_PHASE).mean() - gameview.view(src[..., i], 1, sd, INK_PHASE).mean()))
        se, pe = ends(src[..., i]), ends(tex[..., i]); lev.append((se, pe))
    ps, hs = topo(src, ch); pn, hn = topo(dn, ch)
    return dict(rt=float(err.mean()), bias=float(bias / max(n, 1)), ink=ink, levels=lev, topo=(ps, hs, pn, hn),
                gv_new=gv_rmse(tex, F, res, src, sd, ch), gv_orig=gv_rmse(src, 1, res, src, sd, ch))


def verdict(m):
    """{check: (measured value, text)} of the checks that fail."""
    bad = {}
    ink = max((abs(v) for v in m['ink']), default=0.0)
    if ink > INK: bad['ink'] = (ink, 'game-view mean ink off by ' + '/'.join(f'{v:+.2f}' for v in m['ink']) + f' (limit {INK})')
    lev = max((max(abs(a - b) for a, b in zip(se, pe)) for se, pe in m['levels']), default=0)
    if lev > LEVELS: bad['levels'] = (lev, 'plateau ends source/pack ' + '; '.join(f'{se[0]}/{se[1]} vs {pe[0]}/{pe[1]}' for se, pe in m['levels']) + f' (limit {LEVELS})')
    if m['rt'] > RT: bad['rt'] = (m['rt'], f"round-trip mean error {m['rt']:.1f} (limit {RT})")
    d = m['gv_new'] - m['gv_orig']
    if d > GV: bad['gv'] = (d, f"game-view RMSE {m['gv_new']:.1f} vs original {m['gv_orig']:.1f} (limit +{GV})")
    return bad


def summary(m):
    """The gate values as one manifest cell."""
    p = m['topo']
    return (f"rt={m['rt']:.2f} bias={m['bias']:+.3f} ink={'/'.join(f'{v:+.2f}' for v in m['ink'])} "
            f"levels={';'.join(f'{se[0]}/{se[1]}>{pe[0]}/{pe[1]}' for se, pe in m['levels'])} gv={m['gv_new']:.2f}/{m['gv_orig']:.2f} topo={p[0]}/{p[1]}>{p[2]}/{p[3]}")
