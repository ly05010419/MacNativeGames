# Game-view model of a UI texture: the hook's pack texture has ONE mip level, and the UI samples it with linear filtering,
# so each screen pixel is a bilinear sample of the pack texture at its centre (clamp to edge).
# view(tex, F, s, phase): tex = pack texels (float HxW[xC]) at F x the source size; s = screen px per SOURCE texel;
# phase = (px, py) screen offset of the texture's top-left corner in [0,1). Returns the screen image (float).
import numpy as np

def bilinear(tex, u, v):
    """Sample tex at continuous texel coords (u, v) (texel centres at k+0.5), clamp to edge."""
    H, W = tex.shape[:2]
    x = np.clip(u - 0.5, 0, W - 1); y = np.clip(v - 0.5, 0, H - 1)
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int); x1 = np.minimum(x0 + 1, W - 1); y1 = np.minimum(y0 + 1, H - 1)
    fx = x - x0; fy = y - y0
    if tex.ndim == 3: fx = fx[..., None]; fy = fy[..., None]
    return (tex[y0, x0] * (1 - fx) + tex[y0, x1] * fx) * (1 - fy) + (tex[y1, x0] * (1 - fx) + tex[y1, x1] * fx) * fy

def view(tex, F, s, phase=(0.0, 0.0)):
    H, W = tex.shape[:2]; h, w = H / F, W / F                 # source size
    n = int(np.ceil(w * s + phase[0])), int(np.ceil(h * s + phase[1]))
    jx = (np.arange(n[0]) + 0.5 - phase[0]) / s * F; iy = (np.arange(n[1]) + 0.5 - phase[1]) / s * F
    u, v = np.meshgrid(jx, iy)
    return bilinear(tex.astype(float), u, v)
