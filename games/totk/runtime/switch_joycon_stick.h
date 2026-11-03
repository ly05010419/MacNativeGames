/* Left Joy-Con (057e:2006), with the report id included in p[0]. The Mac's 0x3f descriptor has unsigned 16-bit
   X/Y axes (0..65535, centre 0x8000); captured down input changed bytes 6-7 from 00:80 to 00:ff while full-mode
   setup was pending. Convert it to the same 12-bit, Y-up coordinates as 0x30, without treating either layout as
   the other. Button handling and the fallback's activation/native-priority policy are unchanged. */
#pragma once
#include <stddef.h>
#include <stdint.h>

static inline int switch_joycon_left_decode(uint32_t id, const uint8_t *p, size_t length, int *x, int *y) {
    if (!p || !x || !y || length < 12 || p[0] != id) return 0;
    if (id == 0x30) {
        *x = p[6] | (p[7] & 15) << 8;
        *y = (p[7] >> 4) | p[8] << 4;
    } else if (id == 0x3f) {
        unsigned sx = p[4] | (unsigned)p[5] << 8, sy = p[6] | (unsigned)p[7] << 8;
        *x = (int)((sx * 4095u + 32767u) / 65535u);
        *y = 4095 - (int)((sy * 4095u + 32767u) / 65535u);
    } else return 0;
    return 1;
}
