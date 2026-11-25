/* The fixed MetalFX upscale size's math (mfx_target.h): the fit, the "Upscale to" list and the saved choice. */
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "mfx_target.h"

#define CHECK(c) do { if (!(c)) { printf("fail: line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static int fit(unsigned tw, unsigned th, unsigned vw, unsigned vh, unsigned cw, unsigned ch, unsigned ew, unsigned eh) {
    unsigned ow = 0, oh = 0; mfx_target_fit(tw, th, vw, vh, cw, ch, &ow, &oh); return ow == ew && oh == eh;
}
static int stops(unsigned rw, unsigned rh, unsigned n, const uint32_t *want) {
    uint32_t s[5]; if (mfx_target_stops(rw, rh, s) != n) return 0;
    for (unsigned i = 0; i < n; i++) if (s[i] != want[i]) return 0;
    return 1;
}

int main(void) {
    unsigned w, h; mfx_target_unpack(mfx_target_pack(2560, 1440), &w, &h); CHECK(w == 2560 && h == 1440);
    CHECK(fit(5120, 2880, 3840, 2160, 3840, 2160, 5120, 2880));   // today's 5K path
    CHECK(fit(3840, 2160, 2560, 1080, 1920, 1080, 3840, 1620));   // 21:9 window keeps its shape
    CHECK(fit(3840, 2160, 800, 450, 1920, 1080, 3840, 2160));     // window smaller than the render
    CHECK(fit(2560, 1440, 3840, 2160, 1920, 1080, 2560, 1440));   // target under a 4K full screen: the present stretches it
    uint32_t a[] = {0, mfx_target_pack(1920, 1080), mfx_target_pack(2560, 1440), mfx_target_pack(3840, 2160), mfx_target_pack(5120, 2880)};
    CHECK(stops(1280, 720, 5, a));
    uint32_t b[] = {0, mfx_target_pack(2560, 1440), mfx_target_pack(3840, 2160), mfx_target_pack(5120, 2880)};
    CHECK(stops(1920, 1080, 4, b));
    uint32_t c[] = {0, mfx_target_pack(3840, 2160), mfx_target_pack(5120, 2880)};
    CHECK(stops(2560, 1440, 3, c));
    uint32_t d[] = {mfx_target_pack(5120, 2880)};
    CHECK(stops(3840, 2160, 1, d));
    CHECK(stops(5120, 2880, 0, d));
    CHECK(mfx_target_valid(mfx_target_pack(2560, 1440), 2560, 1440) == 0);
    CHECK(mfx_target_valid(0, 3840, 2160) == mfx_target_pack(5120, 2880));
    CHECK(mfx_target_valid(mfx_target_pack(9999, 9999), 1920, 1080) == 0);
    CHECK(mfx_target_valid(mfx_target_pack(3840, 2160), 1920, 1080) == mfx_target_pack(3840, 2160));
    printf("pass: mfx target\n");
    return 0;
}
