#include "../motion_pro.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void word(uint8_t *r, int k, int i, int v) { uint16_t n = (uint16_t)v; r[13 + 12 * k + 2 * i] = n; r[14 + 12 * k + 2 * i] = n >> 8; }
int main(void) {
    pro_motion s = {0}; pro_motion_sample out[3]; uint8_t r[64] = {0x30}; uint64_t t = 1000000000;
    assert(!pro_motion_decode(&s, r, 48, t, out));
    r[0] = 0x21; assert(!pro_motion_decode(&s, r, 49, t, out)); r[0] = 0x30;
    assert(!pro_motion_decode(&s, r, 49, t, out));   // empty sensor stream
    r[1]++;
    for (int k = 0; k < 3; k++) { word(r, k, 0, -4096); word(r, k, 1, 2048); word(r, k, 2, 4096); word(r, k, 3, 100 + k); word(r, k, 4, -200); word(r, k, 5, 300); }
    assert(pro_motion_decode(&s, r, 49, t, out) == 3);
    for (int k = 0; k < 3; k++) {
        assert(out[k].acc[0] == .5f && out[k].acc[1] == 1 && out[k].acc[2] == -1);
        assert(fabsf(out[k].rot[0] - 14.f * (float)M_PI / 180) < 1e-6f);
        assert(fabsf(out[k].rot[1] - (100 + k) * .070f * (float)M_PI / 180) < 1e-6f);
        assert(fabsf(out[k].rot[2] - 21.f * (float)M_PI / 180) < 1e-6f);
        assert(out[k].ns == t - (2 - k) * PRO_MOTION_PERIOD_NS);
    }
    assert(!pro_motion_decode(&s, r, 49, t + 15000000, out));   // duplicate packet
    memset(r + 13, 0, 36); r[1]++;
    assert(pro_motion_decode(&s, r, 64, t + 15000000, out) == 3);   // real free fall after live data
    assert(out[2].acc[2] == 0 && out[2].rot[2] == 0);
    assert(!pro_motion_decode(&s, r, 49, t + PRO_MOTION_STALE_NS + 15000001, out));   // stale stream must become live again
    r[1] = 255; word(r, 2, 2, -32768); word(r, 2, 5, -32768);
    assert(pro_motion_decode(&s, r, 49, t + 400000000, out) == 3 && out[2].acc[2] == 8 && out[2].rot[2] < 0);
    r[1] = 0; assert(pro_motion_decode(&s, r, 49, t + 415000000, out) == 3);   // report counter wrap
    for (int k = 0; k < 3; k++) word(r, k, 5, 20);
    r[1]++; assert(pro_motion_decode(&s, r, 49, t + 430000000, out) == 3 && out[2].rot[2] > 0);   // slow turn is not learned as bias
    assert(pro_motion_player(1, 1, 2) == 2);
    assert(pro_motion_player(1, 1, -1) == -1 && pro_motion_player(0, 1, 0) == -1);
    assert(pro_motion_player(2, 1, 0) == -1 && pro_motion_player(1, 2, 0) == -1);
    puts("t_motion_pro: passed");
}
