#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "switch_joycon_stick.h"
#include "switch_left_fallback.h"

int main(void) {
    const uint8_t centre[] = {0x3f,0,0,8,0,0x80,0,0x80,0,0x80,0,0x80};
    /* Actual failed first-down capture: the input arrived before the slow mode write finished. */
    const uint8_t down[] = {0x3f,0,0,8,0,0x80,0,0xff,0,0x80,0,0x80};
    uint8_t full[] = {0x30,0,0,0,0,0,0,8,0,0,0,0};   /* full-mode X=2048, Y=0 (down) */
    int x, y;
    assert(switch_joycon_left_decode(0x3f, centre, sizeof centre, &x, &y) && x == 2048 && y == 2047);
    assert(switch_joycon_left_decode(0x3f, down, sizeof down, &x, &y) && x == 2048 && y == 16);
    SwitchLeftFallback s = {0}; float fx, fy;
    uint64_t now = 1000000000ULL;
    switch_left_feed(&s, x, y, now);
    assert(!switch_left_get(&s, 0, 0, now, &fx, &fy));
    switch_left_feed(&s, x, y, now + 208000000);
    assert(switch_left_get(&s, 0, 0, now + 208000000, &fx, &fy) && fx == 0 && fy < -0.99f);
    assert(switch_joycon_left_decode(0x30, full, sizeof full, &x, &y) && x == 2048 && y == 0);
    switch_left_feed(&s, x, y, now + 209000000);
    assert(switch_left_get(&s, 0, 0, now + 209000000, &fx, &fy) && fy == -1);   /* no new gate on mode transition */
    assert(switch_joycon_left_decode(0x3f, centre, sizeof centre, &x, &y));
    switch_left_feed(&s, x, y, now + 220000000);
    assert(switch_left_get(&s, 0, 0, now + 220000000, &fx, &fy) && fabsf(fx) < 0.001f && fabsf(fy) < 0.001f);
    assert(!switch_left_get(&s, 0.2f, 0, now + 221000000, &fx, &fy));   /* native input still wins */

    const unsigned values[] = {0, 32768, 65535};
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) {
        uint8_t report[12] = {0x3f};
        report[4] = values[a] & 255; report[5] = values[a] >> 8;
        report[6] = values[b] & 255; report[7] = values[b] >> 8;
        assert(switch_joycon_left_decode(0x3f, report, sizeof report, &x, &y));
        assert(x == (a == 0 ? 0 : a == 1 ? 2048 : 4095));
        assert(y == (b == 0 ? 4095 : b == 1 ? 2047 : 0));
    }
    full[7] = 0xf8; full[8] = 0xff;
    assert(switch_joycon_left_decode(0x30, full, sizeof full, &x, &y) && x == 2048 && y == 4095);
    assert(!switch_joycon_left_decode(0x30, centre, sizeof centre, &x, &y));   /* id/layout mismatch */
    assert(!switch_joycon_left_decode(0x21, centre, sizeof centre, &x, &y));
    assert(!switch_joycon_left_decode(0x3f, centre, 11, &x, &y));
    assert(!switch_joycon_left_decode(0x3f, NULL, 12, &x, &y));
    puts("PASS: captured simple-mode input, full-mode continuity, native priority, axis bounds and malformed reports");
    return 0;
}
