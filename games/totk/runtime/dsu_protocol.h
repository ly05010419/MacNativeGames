/* Cemuhook / DSU protocol 1001: https://v1993.github.io/cemuhook-protocol/ .
   Pure C; no device-specific handshake or dependency on a server's clock epoch.
   Motion is converted to the existing GCMotion frame (G and radians/second). */
#ifndef TOTK_DSU_PROTOCOL_H
#define TOTK_DSU_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {
    char host[256], service[6];
    uint8_t slot;
} DSUConfig;

typedef struct {
    uint32_t buttons, server, sequence;
    uint8_t axes[4], mac[6];
    int connected;
    uint64_t timestamp;
    double acc[3], gyro[3];
} DSUInput;

static inline uint16_t dsu_u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t dsu_u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static inline void dsu_put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static inline uint32_t dsu_crc(const uint8_t *p, size_t n) {
    uint32_t c = 0xffffffff;
    for (size_t i = 0; i < n; i++) {
        c ^= i >= 8 && i < 12 ? 0 : p[i];
        for (int j = 0; j < 8; j++) c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320);
    }
    return ~c;
}
static inline double dsu_float(const uint8_t *p) { uint32_t u = dsu_u32(p); float f; memcpy(&f, &u, 4); return f; }

/* 0 disabled, 1 configured, -1 malformed. Brackets disambiguate IPv6 and port. */
static inline int dsu_config(const char *endpoint, const char *slot, DSUConfig *out) {
    if (!endpoint || !*endpoint || !strcmp(endpoint, "0")) return 0;
    DSUConfig c = {0};
    if (slot && *slot) { if (slot[0] < '0' || slot[0] > '3' || slot[1]) return -1; c.slot = (uint8_t)(slot[0] - '0'); }
    if (!strcmp(endpoint, "1")) endpoint = "127.0.0.1:26760";
    const char *host = endpoint, *port;
    size_t n;
    if (*host == '[') {
        host++;
        const char *end = strchr(host, ']');
        if (!end || end[1] != ':') return -1;
        n = (size_t)(end - host); port = end + 2;
    } else {
        const char *end = strchr(host, ':');
        if (!end || strchr(end + 1, ':')) return -1;
        n = (size_t)(end - host); port = end + 1;
    }
    if (!n || n >= sizeof c.host || !*port) return -1;
    for (size_t i = 0; i < n; i++) if ((unsigned char)host[i] <= 32 || host[i] == '/' || host[i] == '[' || host[i] == ']') return -1;
    unsigned number = 0;
    for (const char *p = port; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        number = number * 10 + (unsigned)(*p - '0');
        if (number > 65535) return -1;
    }
    if (!number) return -1;
    memcpy(c.host, host, n); c.host[n] = 0;
    snprintf(c.service, sizeof c.service, "%u", number);
    *out = c;
    return 1;
}

static inline void dsu_register(uint8_t p[28], uint32_t client, uint8_t slot) {
    memset(p, 0, 28); memcpy(p, "DSUC", 4); p[4] = 0xe9; p[5] = 3; p[6] = 12;
    dsu_put32(p + 12, client); dsu_put32(p + 16, 0x100002); p[20] = 1; p[21] = slot;
    dsu_put32(p + 8, dsu_crc(p, 28));
}

static inline int dsu_decode(const uint8_t *p, size_t n, uint8_t slot, DSUInput *out) {
    if (n < 100 || memcmp(p, "DSUS", 4) || dsu_u16(p + 4) != 1001) return 0;
    size_t size = 16u + dsu_u16(p + 6);
    if (size < 100 || size > n || size > 512 || dsu_u32(p + 16) != 0x100002 ||
        dsu_u32(p + 8) != dsu_crc(p, size) || p[20] != slot || slot > 3) return 0;
    DSUInput next = {0};
    next.connected = p[21] == 2 && p[31] == 1;
    next.server = dsu_u32(p + 12); next.sequence = dsu_u32(p + 32);
    next.timestamp = (uint64_t)dsu_u32(p + 68) | ((uint64_t)dsu_u32(p + 72) << 32);
    memcpy(next.mac, p + 24, sizeof next.mac);
    if (!next.connected) { *out = next; return 1; }
    /* DSU face buttons are Y/B/A/X by position: north/east/south/west.
       Byte 39 is the touchpad click, not a vendor-specific capture button. */
    static const int first[] = {12, 15, 14, 13, 10, 5, 4, 11}, second[] = {3, 1, 0, 2, 7, 6, 9, 8};
    for (int i = 0; i < 8; i++) {
        if (p[36] & (128 >> i)) next.buttons |= 1u << first[i];
        if (p[37] & (128 >> i)) next.buttons |= 1u << second[i];
    }
    if (p[39]) next.buttons |= 1u << 16;
    memcpy(next.axes, p + 40, sizeof next.axes);
    for (int i = 0; i < 3; i++) next.acc[i] = dsu_float(p + 76 + 4 * i);
    const double rad = 3.141592653589793 / 180;
    next.gyro[0] = dsu_float(p + 88) * rad;
    next.gyro[1] = -dsu_float(p + 96) * rad;
    next.gyro[2] = dsu_float(p + 92) * rad;
    for (int i = 0; i < 3; i++) if (!isfinite(next.acc[i]) || !isfinite(next.gyro[i])) return 0;
    *out = next;
    return 1;
}

static inline int dsu_sequence_after(uint32_t next, uint32_t previous) {
    uint32_t delta = next - previous;
    return delta && delta < 0x80000000u;
}

typedef struct { uint64_t remote, local; int valid; } DSUClock;
/* Preserve server sample intervals, anchored to this process's monotonic clock.
   0 duplicate/backlogged, 1 sample, 2 new epoch (reset the fusion before feed).
   DSU timestamps may be uptime or wall time on a completely different machine. */
static inline int dsu_sample_time(DSUClock *clock, uint64_t remote, uint64_t now, uint64_t *stamp) {
    if (clock->valid && remote == clock->remote) return 0;
    int reset = !clock->valid || remote < clock->remote || remote - clock->remote > 250000;
    uint64_t local = now;
    if (!reset) {
        uint64_t delta = (remote - clock->remote) * 1000;
        if (clock->local > UINT64_MAX - delta) return 0;
        local = clock->local + delta;
        if (local > now && local - now > 5000000ull) { local = now; reset = 1; }
        if (now > local && now - local > 250000000ull) return 0;
    }
    *clock = (DSUClock){remote, local, 1}; *stamp = local;
    return reset ? 2 : 1;
}
#endif
