#include <assert.h>
#include <stdio.h>
#include "../shared/runtime/switch_left_fallback.h"

static void pack_9_left(uint8_t out[9], int x_max, int y_max, int x_center, int y_center, int x_min, int y_min) {
    out[0] = x_max & 0xff;
    out[1] = ((x_max >> 8) & 0x0f) | ((y_max & 0x0f) << 4);
    out[2] = (y_max >> 4) & 0xff;
    out[3] = x_center & 0xff;
    out[4] = ((x_center >> 8) & 0x0f) | ((y_center & 0x0f) << 4);
    out[5] = (y_center >> 4) & 0xff;
    out[6] = x_min & 0xff;
    out[7] = ((x_min >> 8) & 0x0f) | ((y_min & 0x0f) << 4);
    out[8] = (y_min >> 4) & 0xff;
}

static void pack_9_right(uint8_t out[9], int x_center, int y_center, int x_min, int y_min, int x_max, int y_max) {
    out[0] = x_center & 0xff;
    out[1] = ((x_center >> 8) & 0x0f) | ((y_center & 0x0f) << 4);
    out[2] = (y_center >> 4) & 0xff;
    out[3] = x_min & 0xff;
    out[4] = ((x_min >> 8) & 0x0f) | ((y_min & 0x0f) << 4);
    out[5] = (y_min >> 4) & 0xff;
    out[6] = x_max & 0xff;
    out[7] = ((x_max >> 8) & 0x0f) | ((y_max & 0x0f) << 4);
    out[8] = (y_max >> 4) & 0xff;
}

int main(void) {
    /* Betop Zeus NS Bluetooth match */
    assert(betop_ns_match(0x057e, 0x2009, "Bluetooth", "00:90:E0:12:34:56"));
    assert(betop_ns_match(0x057e, 0x2009, "bluetooth", "00-90-e0-aa-bb-cc"));
    assert(!betop_ns_match(0x057e, 0x2009, "USB", "00:90:E0:12:34:56"));
    assert(!betop_ns_match(0x057e, 0x2006, "Bluetooth", "00:90:E0:12:34:56"));
    assert(!betop_ns_match(0x054c, 0x05c4, "Bluetooth", "00:90:E0:12:34:56"));
    assert(!betop_ns_match(0x057e, 0x2009, "Bluetooth", "11:90:E0:12:34:56"));
    assert(!betop_ns_match(0x057e, 0x2009, "Bluetooth", "00:90-E0:12:34:56"));
    assert(!betop_ns_match(0x057e, 0x2009, "Bluetooth", "00:90:E0:12:34"));
    assert(!betop_ns_match(0x057e, 0x2009, "Bluetooth", "00:90:E0:12:34:ZZ"));
    assert(!betop_ns_match(0x057e, 0x2009, NULL, "00:90:E0:12:34:56"));
    assert(!betop_ns_match(0x057e, 0x2009, "Bluetooth", NULL));

    /* Real captured Betop Zeus NS calibration: center=(2020,2013), X +1480/-1641, Y +1560/-1478 */
    uint8_t left_raw[9], right_raw[9], bad_ff[9], bad_zero[9] = {0};
    for (int i = 0; i < 9; i++) bad_ff[i] = 0xff;
    pack_9_left(left_raw, 1480, 1560, 2020, 2013, 1641, 1478);
    pack_9_right(right_raw, 2122, 1920, 1600, 1500, 1550, 1580);

    SwitchStickCal lc = {0}, rc = {0};
    assert(switch_stick_cal_parse_left(left_raw, &lc));
    assert(lc.valid && lc.x_center == 2020 && lc.y_center == 2013);
    assert(lc.x_max == 1480 && lc.x_min == 1641 && lc.y_max == 1560 && lc.y_min == 1478);
    assert(switch_stick_cal_parse_right(right_raw, &rc));
    assert(rc.valid && rc.x_center == 2122 && rc.y_center == 1920);
    assert(rc.x_min == 1600 && rc.y_min == 1500 && rc.x_max == 1550 && rc.y_max == 1580);
    assert(!switch_stick_cal_parse_left(bad_ff, &lc));
    assert(!switch_stick_cal_parse_left(bad_zero, &lc));
    assert(!switch_stick_cal_parse_right(bad_ff, &rc));
    assert(!switch_stick_cal_parse_right(bad_zero, &rc));

    /* Axis scaling & clamping with parsed calibration */
    assert(fabsf(switch_stick_cal_axis(2020, lc.x_center, lc.x_min, lc.x_max)) < 1e-6f);
    assert(fabsf(switch_stick_cal_axis(2020 + 1480, lc.x_center, lc.x_min, lc.x_max) - 1.0f) < 1e-6f);
    assert(fabsf(switch_stick_cal_axis(2020 - 1641, lc.x_center, lc.x_min, lc.x_max) + 1.0f) < 1e-6f);
    assert(switch_stick_cal_axis(4095, lc.x_center, lc.x_min, lc.x_max) == 1.0f);
    assert(switch_stick_cal_axis(0, lc.x_center, lc.x_min, lc.x_max) == -1.0f);

    /* Immediate Betop fallback activates on first report, uses calibration, yields to native, expires at >250ms */
    SwitchLeftFallback fb = {0};
    uint64_t t = 1000000000ULL;
    switch_left_configure(&fb, &lc, 1);
    float x = 0.f, y = 0.f;
    switch_left_feed(&fb, 2020 + 740, 2013 - 739, t);
    assert(switch_left_get(&fb, 0.f, 0.f, t, &x, &y) == 1);
    assert(fabsf(x - 0.5f) < 1e-4f && fabsf(y + 0.5f) < 1e-4f);
    /* Native input non-zero abandons fallback cleanly */
    assert(switch_left_get(&fb, 0.25f, 0.f, t + 1000000ULL, &x, &y) == 0);
    assert(!fb.active);
    /* Stale HID (>250ms) deactivates fallback */
    switch_left_feed(&fb, 2020 + 1480, 2013, t + 10000000ULL);
    assert(switch_left_get(&fb, 0.f, 0.f, t + 10000000ULL, &x, &y) == 1);
    assert(switch_left_get(&fb, 0.f, 0.f, t + 261000001ULL, &x, &y) == 0);

    puts("PASS: betop_ns_match, SPI stick calibration, immediate calibrated fallback");
    return 0;
}
