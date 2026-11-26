/* Some Switch-compatible controllers send live 12-bit stick reports while
   GameController leaves the left stick at zero. Use raw input only after a
   sustained discrepancy (or immediately for a verified Betop Zeus NS device),
   and abandon it as soon as native input works. */
#pragma once
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int x_center, y_center, x_min, x_max, y_min, y_max, valid;
} SwitchStickCal;

typedef struct {
    int x, y, valid, active, immediate;
    uint64_t at, zeroSince;
    SwitchStickCal cal;
} SwitchLeftFallback;

static inline int switch_ascii_eq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static inline void switch_stick_cal_default(SwitchStickCal *c) {
    if (!c) return;
    *c = (SwitchStickCal){2048, 2048, 2048, 2047, 2048, 2047, 0};
}

static inline int switch_stick_cal_valid(const SwitchStickCal *c) {
    return c &&
        c->x_center >= 1024 && c->x_center <= 3072 &&
        c->y_center >= 1024 && c->y_center <= 3072 &&
        c->x_min >= 256 && c->x_min <= 2048 &&
        c->x_max >= 256 && c->x_max <= 2048 &&
        c->y_min >= 256 && c->y_min <= 2048 &&
        c->y_max >= 256 && c->y_max <= 2048;
}

/* Nintendo Switch Pro SPI stick calibration (9 bytes): left at 0x603D / 0x8012,
   order is x_max, y_max, x_center, y_center, x_min, y_min (12-bit packed). */
static inline int switch_stick_cal_parse_left(const uint8_t p[9], SwitchStickCal *out) {
    if (!p || !out) return 0;
    SwitchStickCal c = {0};
    c.x_max    = p[0] | ((p[1] & 0x0f) << 8);
    c.y_max    = (p[1] >> 4) | (p[2] << 4);
    c.x_center = p[3] | ((p[4] & 0x0f) << 8);
    c.y_center = (p[4] >> 4) | (p[5] << 4);
    c.x_min    = p[6] | ((p[7] & 0x0f) << 8);
    c.y_min    = (p[7] >> 4) | (p[8] << 4);
    if (!switch_stick_cal_valid(&c)) return 0;
    c.valid = 1;
    *out = c;
    return 1;
}

/* Nintendo Switch Pro SPI stick calibration (9 bytes): right at 0x6046 / 0x801D,
   order is x_center, y_center, x_min, y_min, x_max, y_max (12-bit packed). */
static inline int switch_stick_cal_parse_right(const uint8_t p[9], SwitchStickCal *out) {
    if (!p || !out) return 0;
    SwitchStickCal c = {0};
    c.x_center = p[0] | ((p[1] & 0x0f) << 8);
    c.y_center = (p[1] >> 4) | (p[2] << 4);
    c.x_min    = p[3] | ((p[4] & 0x0f) << 8);
    c.y_min    = (p[4] >> 4) | (p[5] << 4);
    c.x_max    = p[6] | ((p[7] & 0x0f) << 8);
    c.y_max    = (p[7] >> 4) | (p[8] << 4);
    if (!switch_stick_cal_valid(&c)) return 0;
    c.valid = 1;
    *out = c;
    return 1;
}

/* Betop Zeus in NS mode reports Nintendo VID/PID (057E:2009) over Bluetooth with
   OUI prefix 00:90:E0 while macOS GameController leaves its left stick at zero. */
static inline int betop_ns_match(int vid, int pid, const char *transport, const char *address) {
    if (vid != 0x057e || pid != 0x2009 || !switch_ascii_eq_ci(transport, "Bluetooth") ||
        !address || strlen(address) != 17) return 0;
    const char sep = address[2];
    if (sep != ':' && sep != '-') return 0;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2 ? address[i] != sep : !isxdigit((unsigned char)address[i])) return 0;
    }
    return tolower((unsigned char)address[0]) == '0' && tolower((unsigned char)address[1]) == '0' &&
           tolower((unsigned char)address[3]) == '9' && tolower((unsigned char)address[4]) == '0' &&
           tolower((unsigned char)address[6]) == 'e' && tolower((unsigned char)address[7]) == '0';
}

static inline void switch_left_feed(SwitchLeftFallback *s, int x, int y, uint64_t now) {
    s->valid = x >= 0 && x <= 4095 && y >= 0 && y <= 4095;
    s->x = x; s->y = y; s->at = now;
}

static inline void switch_left_configure(SwitchLeftFallback *s, const SwitchStickCal *cal, int immediate) {
    if (!s) return;
    if (cal && switch_stick_cal_valid(cal)) {
        s->cal = *cal;
        s->cal.valid = 1;
    }
    s->immediate = immediate ? 1 : 0;
}

static inline float switch_left_axis(int raw) {
    int delta = raw - 2048;
    return delta / (delta < 0 ? 2048.0f : 2047.0f);
}

static inline float switch_stick_cal_axis(int raw, int center, int min_below, int max_above) {
    int delta = raw - center;
    int span = delta < 0 ? min_below : max_above;
    if (span <= 0) return switch_left_axis(raw);
    float v = (float)delta / (float)span;
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

static inline int switch_left_get(SwitchLeftFallback *s, float nativeX, float nativeY,
                                  uint64_t now, float *x, float *y) {
    if (!s->valid || now < s->at || now - s->at > 250000000ULL) {
        s->active = 0; s->zeroSince = 0; return 0;
    }
    if (fabsf(nativeX) > 0.0001f || fabsf(nativeY) > 0.0001f) {
        s->active = 0; s->zeroSince = 0; return 0;
    }
    if (s->cal.valid) {
        *x = switch_stick_cal_axis(s->x, s->cal.x_center, s->cal.x_min, s->cal.x_max);
        *y = switch_stick_cal_axis(s->y, s->cal.y_center, s->cal.y_min, s->cal.y_max);
    } else {
        *x = switch_left_axis(s->x);
        *y = switch_left_axis(s->y);
    }
    if (s->immediate) {
        s->active = 1;
        return 1;
    }
    if (fmaxf(fabsf(*x), fabsf(*y)) > 0.5f) {
        if (!s->zeroSince) s->zeroSince = now;
        if (now - s->zeroSince >= 200000000ULL) s->active = 1;
    } else s->zeroSince = 0;
    return s->active;
}
