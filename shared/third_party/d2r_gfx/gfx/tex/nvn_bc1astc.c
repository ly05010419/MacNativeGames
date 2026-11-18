// See nvn_bc1astc.h. ASTC layout used (one partition, 4x4 weight grid, endpoints always 8-bit, so the integer sequence
// encoding is plain bits):
//   bits 0-10   block mode: 0x042 = 2-bit weights, 0x053 = 3-bit weights, 0x441 = dual plane with 1-bit weights
//   bits 11-12  partition count - 1 = 0
//   bits 13-16  colour endpoint mode: 8 = LDR RGB direct, 12 = LDR RGBA direct
//   bits 17-    endpoint values v0, v1, ... (r0 r1 g0 g1 b0 b1 [a0 a1]), 8 bits each
//   top bits    weights, stored bit-reversed from bit 127 down; dual plane: interleaved plane0/plane1 per texel and the
//               2-bit colour component selector (3 = alpha) just below them
// ASTC swaps the endpoints (blue contraction) when the second endpoint's r+g+b is smaller; endpoints are ordered so
// that this never happens, inverting the weights when they are swapped.
#include "nvn_bc1astc.h"
#include <string.h>

static void put(uint8_t *b, unsigned pos, unsigned n, uint32_t v) {
    for (unsigned i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) >> 3] |= (uint8_t)(1u << ((pos + i) & 7));
}
static void put_weight_bits(uint8_t *b, unsigned pos, unsigned n, uint32_t v) {   // weight stream bit j lives at block bit 127 - j
    for (unsigned i = 0; i < n; i++) if (v >> i & 1) { unsigned bit = 127 - (pos + i); b[bit >> 3] |= (uint8_t)(1u << (bit & 7)); }
}
static int expand5(int v) { return v << 3 | v >> 2; }
static int expand6(int v) { return v << 2 | v >> 4; }

// Writes the block. e0/e1: RGBA endpoints (alpha used only when cem == 12); w: weights (dual plane: plane 0 then plane 1
// per texel); wbits: bits per weight.
static void emit(uint8_t *out, int cem, int dual, int wbits, int e0[4], int e1[4], int w[32]) {
    int nw = dual ? 32 : 16, wmax = (1 << wbits) - 1;
    if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2]) {   // keep r+g+b of the second endpoint >= the first: no blue contraction
        for (int c = 0; c < 4; c++) { int t = e0[c]; e0[c] = e1[c]; e1[c] = t; }
        for (int i = 0; i < nw; i++) w[i] = wmax - w[i];
    }
    memset(out, 0, 16);
    unsigned mode = dual ? 0x441u : wbits == 2 ? 0x042u : 0x053u;
    put(out, 0, 11, mode);
    put(out, 13, 4, (uint32_t)cem);
    unsigned pos = 17;
    int nc = cem == 12 ? 4 : 3;
    for (int c = 0; c < nc; c++) { put(out, pos, 8, (uint32_t)e0[c]); put(out, pos + 8, 8, (uint32_t)e1[c]); pos += 16; }
    for (int i = 0; i < nw; i++) put_weight_bits(out, (unsigned)(i * wbits), (unsigned)wbits, (uint32_t)w[i]);
    if (dual) put(out, 128 - (unsigned)(nw * wbits) - 2, 2, 3u);   // colour component selector: alpha on plane 1
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

void nvn_bc1_to_astc(const uint8_t bc[8], uint8_t out[16], int rgba_formats) {
    int c0 = bc[0] | bc[1] << 8, c1 = bc[2] | bc[3] << 8;
    uint32_t ix = (uint32_t)bc[4] | (uint32_t)bc[5] << 8 | (uint32_t)bc[6] << 16 | (uint32_t)bc[7] << 24;
    int p0[3] = {expand5(c0 >> 11), expand6(c0 >> 5 & 63), expand5(c0 & 31)};
    int p1[3] = {expand5(c1 >> 11), expand6(c1 >> 5 & 63), expand5(c1 & 31)};
    int idx[16], used = 0;
    for (int k = 0; k < 16; k++) { idx[k] = (int)(ix >> (2 * k) & 3); used |= 1 << idx[k]; }
    int e0[4] = {p0[0], p0[1], p0[2], 255}, e1[4] = {p1[0], p1[1], p1[2], 255}, w[32];
    if (c0 > c1) {   // four colours: c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1  ->  weights 0, 3, 1, 2
        static const int map[4] = {0, 3, 1, 2};
        for (int k = 0; k < 16; k++) w[k] = map[idx[k]];
        emit(out, 8, 0, 2, e0, e1, w);
        return;
    }
    if (!(used & 8)) {   // three colours, none transparent: c0, c1, midpoint
        // Weight triple (c0, midpoint, c1) from the 2-bit or 3-bit weight set, endpoints solved so that c0 and c1 land on
        // their weights; the triple and rounding with the smallest squared error over the block's texels wins.
        static const int unq[2][8] = {{0, 21, 43, 64}, {0, 9, 18, 27, 37, 46, 55, 64}};
        int cnt[3] = {0, 0, 0}; for (int k = 0; k < 16; k++) cnt[idx[k]]++;
        int mid[3] = {(p0[0] + p1[0] + 1) / 2, (p0[1] + p1[1] + 1) / 2, (p0[2] + p1[2] + 1) / 2};
        long best = -1; int bb = 2, bwa = 0, bwb = 1, bwc = 3, be0[3] = {0}, be1[3] = {0};
        for (int bits = 2; bits <= 3; bits++) {
            int n = 1 << bits; const int *u = unq[bits - 2];
            for (int wa = 0; wa < n; wa++) for (int wb = wa + 1; wb < n; wb++) for (int wc = wb + 1; wc < n; wc++) {
                int f0[3], f1[3], ok = 1; long err = 0;
                for (int c = 0; c < 3 && ok; c++) {
                    // E0 + u[wa]/64 L = c0, E0 + u[wc]/64 L = c1  ->  L = 64 d / (u[wc] - u[wa])
                    int d = p1[c] - p0[c], span = u[wc] - u[wa];
                    int L = (64 * d + (d >= 0 ? span / 2 : -span / 2)) / span;
                    f0[c] = p0[c] - (u[wa] * L + (L >= 0 ? 32 : -32)) / 64; f1[c] = f0[c] + L;
                    if (f0[c] < 0 || f0[c] > 255 || f1[c] < 0 || f1[c] > 255) ok = 0;
                    int want[3] = {p0[c], p1[c], mid[c]}, wt[3] = {u[wa], u[wc], u[wb]};
                    for (int i = 0; ok && i < 3; i++) { int r = ((f0[c] * 257 * (64 - wt[i]) + f1[c] * 257 * wt[i] + 32) >> 6) >> 8; long e = r - want[i]; err += e * e * cnt[i]; }
                }
                if (ok && (best < 0 || err < best)) { best = err; bb = bits; bwa = wa; bwb = wb; bwc = wc; memcpy(be0, f0, sizeof f0); memcpy(be1, f1, sizeof f1); }
            }
        }
        int f0[4] = {be0[0], be0[1], be0[2], 255}, f1[4] = {be1[0], be1[1], be1[2], 255};
        if (best < 0) { memcpy(f0, e0, sizeof f0); memcpy(f1, e1, sizeof f1); bb = 3; bwa = 0; bwb = 3; bwc = 7; }   // cannot happen: (0, *, max) always fits
        const int map[3] = {bwa, bwc, bwb};
        for (int k = 0; k < 16; k++) w[k] = map[idx[k]];
        emit(out, 8, 0, bb, f0, f1, w);
        return;
    }
    // Three colours with index 3: transparent black (RGBA formats) or opaque black (RGB formats).
    int pal[4][3] = {{p0[0], p0[1], p0[2]}, {p1[0], p1[1], p1[2]}, {(p0[0] + p1[0] + 1) / 2, (p0[1] + p1[1] + 1) / 2, (p0[2] + p1[2] + 1) / 2}, {0, 0, 0}};
    int colours = used & (rgba_formats ? 7 : 15);   // palette entries the colour plane has to show
    // Two endpoints out of the used colours, chosen for the smallest squared error over the block's texels.
    int best_a = 0, best_b = 1; long best = -1;
    for (int a = 0; a < 4; a++) for (int b = a; b < 4; b++) {
        if (!(colours >> a & 1) || !(colours >> b & 1)) continue;
        long err = 0;
        for (int k = 0; k < 16; k++) {
            int i = idx[k]; if (!(colours >> i & 1)) continue;
            long da = 0, db = 0;
            for (int c = 0; c < 3; c++) { int x = pal[i][c] - pal[a][c], y = pal[i][c] - pal[b][c]; da += x * x; db += y * y; }
            err += da < db ? da : db;
        }
        if (best < 0 || err < best) { best = err; best_a = a; best_b = b; }
    }
    int f0[4] = {pal[best_a][0], pal[best_a][1], pal[best_a][2], 255}, f1[4] = {pal[best_b][0], pal[best_b][1], pal[best_b][2], rgba_formats ? 0 : 255};
    if (!rgba_formats) f0[3] = 255;
    for (int k = 0; k < 16; k++) {
        int i = idx[k], cw = 0;
        if (colours >> i & 1) {
            long da = 0, db = 0;
            for (int c = 0; c < 3; c++) { int x = pal[i][c] - f0[c], y = pal[i][c] - f1[c]; da += x * x; db += y * y; }
            cw = db < da;
        }
        w[2 * k] = cw;                                     // plane 0: colour
        w[2 * k + 1] = rgba_formats && i == 3;            // plane 1: alpha (endpoint alpha 255 -> 0)
    }
    if (!rgba_formats) { f1[3] = 255; }
    emit(out, 12, 1, 1, f0, f1, w);
    (void)clamp255;
}

void nvn_bc1_to_astc_image(const uint8_t *src, uint64_t src_pitch, uint32_t bw, uint32_t bh, uint8_t *dst, int rgba_formats) {
    for (uint32_t y = 0; y < bh; y++) for (uint32_t x = 0; x < bw; x++)
        nvn_bc1_to_astc(src + y * src_pitch + x * 8, dst + ((uint64_t)y * bw + x) * 16, rgba_formats);
}
