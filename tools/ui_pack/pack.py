"""The pack picture of one key from its shapes (v3's build3.py render stage).
Pack texture: F x source (8 if max side <= 128, 4 if <= 256, else 2), box pre-filter of F/s_d texels (s_d = the key's draw scale,
screen px per source texel): bilinear sampling at s_d then shows a ~1-screen-px anti-aliased edge."""
import numpy as np
from PIL import Image
from scipy import ndimage

from tools.ui_pack import recon


def factor(src):
    m = max(src.shape[:2])
    return 8 if m <= 128 else 4 if m <= 256 else 2


def used_channels(a):
    """Channels that carry a shape: more than 24 levels between the darkest and the brightest texel."""
    return [i for i in range(a.shape[2]) if np.ptp(a[..., i]) > 24]


def render(src, res, F, sd, o):
    """src float (H, W, C), res the shapes (recon.fit_key), F the factor, sd the draw scale, o the key's options.
    Returns (img uint8 (H*F, W*F, C), dn, npatch): dn is the exact render (width 1) area-downscaled to the source, seen through the
    layer's blur (the gate's round trip); npatch the number of source-patch pixels."""
    H, W, Cn = src.shape; ch = used_channels(src)
    tex = recon.render(res, src, F, F / sd)
    ex = recon.render(res, src, 8 if max(H, W) <= 256 else 4, 1.0); kx = ex.shape[0] // H
    dn = ex.reshape(H, kx, W, kx, Cn).mean(axis=(1, 3))
    for i in ch:   # compare through the source's own blur (layer sig) where the fit used one
        sg = max([l.get('sig', 0.0) for l in res[i].get('layers', [])] or [0.0], default=0.0)
        if sg > 0: dn[..., i] = ndimage.gaussian_filter(dn[..., i], sg, mode='nearest')
    # Source patch: where the traced shape misses the source by a clear margin (a thin tip or faint line the trace dropped, a
    # speck it added), add the source's own residual back, bilinear at F, so such a spot looks like the source, never cut.
    # Weight 0 below t0 of the channel range (edge wobble stays crisp), 1 above t1. Levels stay within the source's range.
    t0, t1 = o.get('patch', [0.08, 0.2]); npatch = 0
    for i in ch:
        lo, hi = float(src[..., i].min()), float(src[..., i].max()); R = src[..., i] - dn[..., i]
        w = np.clip((np.abs(R) / max(hi - lo, 1) - t0) / (t1 - t0), 0, 1)
        for y0, y1, x0, x1, j in o.get('pix', []):
            if j == i: w[max(0, y0 - 1):y1 + 1, max(0, x0 - 1):x1 + 1] = 0   # pasted as source pixels below
        npatch += int((w > 0.5).sum())
        if w.any():   # smoothed (sig 0.6): a paired-row or block-stepped source must not come back as stairs or dots
            tex[..., i] = np.clip(tex[..., i] + recon.up(ndimage.gaussian_filter(w * R, o.get('patch_sig', 0.6), mode='nearest'), F, Image.BILINEAR), lo, hi)
    for y0, y1, x0, x1, i in o.get('pix', []):   # pixel-art parts (1-px arms, single-pixel cores): source pixels as exact squares
        nn = ndimage.uniform_filter(np.kron(src[..., i], np.ones((F, F))), max(1, int(round(F / sd))), mode='nearest')
        h = max(1, int(round(F / sd / 2))); yy = np.arange(y0 * F, y1 * F)[:, None]; xx = np.arange(x0 * F, x1 * F)[None, :]
        m = np.clip(np.minimum(np.minimum(yy - y0 * F, y1 * F - 1 - yy), np.minimum(xx - x0 * F, x1 * F - 1 - xx)) / h, 0, 1)   # feathered edge
        blk = tex[y0 * F:y1 * F, x0 * F:x1 * F, i]; tex[y0 * F:y1 * F, x0 * F:x1 * F, i] = m * nn[y0 * F:y1 * F, x0 * F:x1 * F] + (1 - m) * blk
    return np.clip(np.round(tex), 0, 255).astype(np.uint8), dn, npatch
