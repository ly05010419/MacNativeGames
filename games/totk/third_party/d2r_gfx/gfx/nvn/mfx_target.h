/* The fixed size MetalFX upscales the game picture to ("Upscale to" in the settings panel). Plain C, no Metal, so a test
   can build it. A target is packed as w << 16 | h; 0 means the window (the viewport), as before. */
#ifndef MFX_TARGET_H
#define MFX_TARGET_H
#include <math.h>
#include <stdint.h>

static const unsigned mfx_target_sizes[4][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}, {5120, 2880}};

static inline uint32_t mfx_target_pack(unsigned w, unsigned h) { return (uint32_t)(w << 16 | h); }
static inline void mfx_target_unpack(uint32_t t, unsigned *w, unsigned *h) { *w = t >> 16; *h = t & 0xffffu; }

/* The viewport's shape at the largest size that fits tw x th (the 5K branch of output_upscale with 5120 x 2880 as tw x th).
   Fitting that would leave the output no bigger than the picture (cw + 16, ch + 9, the enlargement margin) falls back to
   the whole tw x th and lets the present stretch it. */
static inline void mfx_target_fit(unsigned tw, unsigned th, unsigned vw, unsigned vh, unsigned cw, unsigned ch, unsigned *ow, unsigned *oh) {
    *ow = tw; *oh = th;
    if (!vw || !vh) return;
    double scale = fmin((double)tw / vw, (double)th / vh);
    unsigned w = (unsigned)llround(vw * scale), h = (unsigned)llround(vh * scale);
    if (w < cw + 16 || h < ch + 9) return;
    *ow = w; *oh = h;
}

/* The choices for a render of rw x rh: the window first (not from 4K up, where it is no better than 5K), then every fixed
   size wider than the render. Returns the count, 0 at 5K (nothing to choose). */
static inline unsigned mfx_target_stops(unsigned rw, unsigned rh, uint32_t out[5]) {
    (void)rh;
    unsigned n = 0;
    if (rw < 3840) out[n++] = 0;
    for (int i = 0; i < 4; i++) if (mfx_target_sizes[i][0] > rw) out[n++] = mfx_target_pack(mfx_target_sizes[i][0], mfx_target_sizes[i][1]);
    return n;
}

/* A saved choice that the render no longer offers (the user raised the render size) becomes the first stop. */
static inline uint32_t mfx_target_valid(uint32_t t, unsigned rw, unsigned rh) {
    uint32_t s[5]; unsigned n = mfx_target_stops(rw, rh, s);
    for (unsigned i = 0; i < n; i++) if (s[i] == t) return t;
    return n ? s[0] : 0;
}
#endif
