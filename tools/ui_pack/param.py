"""Parametric shapes for glyphs whose exact geometry is known: least-squares fit of the parameters (and the source's blur)
so that the box-AA'd, blurred shape matches the source coverage. Used through the key option {"param": "<name>"}."""
import numpy as np; from scipy import ndimage, optimize
from tools.ui_pack import raster

def n_poly(p):
    """N glyph: stems [L0,L1] and [R0,R1] from T to B; diagonal band between x = au + su*y (upper/right edge) and x = al + sl*y."""
    L0, L1, R0, R1, T, B, au, su, al, sl = p
    yu = (R0 - au) / su; yl = (L1 - al) / sl          # where the upper edge meets the right stem, the lower edge the left stem
    return [np.array([(L0, T), (au + su * T, T), (R0, yu), (R0, T), (R1, T), (R1, B), (al + sl * B, B), (L1, yl), (L1, B), (L0, B)])]

SHAPES = {'N': (n_poly, [4.62, 7.49, 16.51, 19.45, 2.9, 19.4, 6.35, 0.667, 2.2, 0.73])}

def fit(C, name):
    f, p0 = SHAPES[name]; H, W = C.shape
    def model(q):
        sig = abs(q[-1]); cv = raster.coverage(f(q[:-1]), H, W, 1, 16)
        return ndimage.gaussian_filter(cv, sig, mode='nearest') if sig > 0.05 else cv
    loss = lambda q: float(((model(q) - C) ** 2).sum())
    r = optimize.minimize(loss, np.r_[p0, 0.4], method='Powell', options=dict(maxiter=20000, xtol=1e-3, ftol=1e-7))
    q = r.x; e = model(q) - C
    return f(q[:-1]), e, abs(q[-1]), q
