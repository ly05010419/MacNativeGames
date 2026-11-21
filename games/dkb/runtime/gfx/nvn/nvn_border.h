// Border colours Metal has no sampler state for (pure function, unit test tests/t_border.m). NVN takes any RGBA colour, Metal has three (MTLSamplerBorderColor 0 transparent black,
// 1 opaque black, 2 opaque white). The sampler gets the nearest, bn. When the colour B is none of them, a wrap mode reaches the border and the sampler filters linearly (a min / max
// reduction does not), a twin with another Metal colour bw lets the shader correct its samples, r = s_n + (s_w - s_n) k with k = (B - Bn) / (Bw - Bn) per channel
// (dmsh_build.py border_emu): exact, since a filtered sample is linear in the border colour. bw must differ from bn in every channel where B does; when no colour does that beside
// the nearest one, bn becomes transparent black and bw opaque white, which differ in all four. Tears of the Kingdom's grass bend map samples with border (0.5, 0.5, 0.5, 0.5)
#pragma once
#include <stdint.h>
#include <string.h>

static const float nvn_mtl_border[3][4] = { {0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1} };
// B: NVN's border colour; wrap: the s, t, r wrap modes (NVN: 4 mirror-clamp-to-border, 5 clamp-to-border); reduction: NVN's (1 min, 2 max). → whether a twin is needed; bn always,
// bw and k only then
static inline int nvn_border(const float B[4], const uint8_t wrap[3], int reduction, int *bn, int *bw, float k[4]) {
    *bn = B[3] > 0.5f ? (B[0] > 0.5f ? 2 : 1) : 0;
    for (int c = 0; c < 3; c++) if (!memcmp(B, nvn_mtl_border[c], sizeof nvn_mtl_border[c])) { *bn = c; return 0; }
    int border = 0; for (int i = 0; i < 3; i++) border |= wrap[i] == 4 || wrap[i] == 5;
    if (!border || reduction == 1 || reduction == 2) return 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) *bn = 0;
        for (int c = 0; c < 3; c++) { if (c == *bn) continue; int ok = 1;
            for (int ch = 0; ch < 4; ch++) ok &= nvn_mtl_border[c][ch] != nvn_mtl_border[*bn][ch] || B[ch] == nvn_mtl_border[*bn][ch];
            if (!ok) continue;
            *bw = c;
            for (int ch = 0; ch < 4; ch++) { float d = nvn_mtl_border[c][ch] - nvn_mtl_border[*bn][ch]; k[ch] = d ? (B[ch] - nvn_mtl_border[*bn][ch]) / d : 0; }
            return 1; }
    }
    return 0;   // not reached: transparent black and opaque white differ in every channel
}
