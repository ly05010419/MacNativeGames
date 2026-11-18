#include "../../runtime/dsu_protocol.h"
#include <assert.h>

static void put_float(uint8_t *p, float f) { uint32_t u; memcpy(&u, &f, 4); dsu_put32(p, u); }
static void seal(uint8_t *p, size_t n) { dsu_put32(p + 8, dsu_crc(p, n)); }
static void frame(uint8_t p[100]) {
    memset(p, 0, 100); memcpy(p, "DSUS", 4); p[4] = 0xe9; p[5] = 3; p[6] = 84;
    dsu_put32(p + 12, 42); dsu_put32(p + 16, 0x100002); p[20] = 2; p[21] = 2; p[31] = 1;
    p[39] = 1; p[40] = 0; p[41] = 128; p[42] = 255; p[43] = 128;
    put_float(p + 76, .25f); put_float(p + 80, -.5f); put_float(p + 84, -1);
    put_float(p + 88, 180); put_float(p + 92, 90); put_float(p + 96, -45); seal(p, 100);
}
int main(void) {
    DSUConfig c;
    assert(dsu_config(NULL, NULL, &c) == 0 && dsu_config("0", NULL, &c) == 0);
    assert(dsu_config("1", NULL, &c) == 1 && !strcmp(c.host, "127.0.0.1") && !strcmp(c.service, "26760") && c.slot == 0);
    assert(dsu_config("server.local:1234", "3", &c) == 1 && !strcmp(c.host, "server.local") && c.slot == 3);
    assert(dsu_config("[::1]:26760", "1", &c) == 1 && !strcmp(c.host, "::1"));
    const char *bad[] = {"localhost", ":26760", "a:0", "a:-1", "a:65536", "a:999999999999999", "a:x", "[::1]", "[::1]:", "::1:26760", "bad host:1"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) assert(dsu_config(bad[i], NULL, &c) == -1);
    assert(dsu_config("1", "4", &c) == -1 && dsu_config("1", "00", &c) == -1);
    uint8_t request[28]; dsu_register(request, 0x12345678, 3);
    assert(!memcmp(request, "DSUC", 4) && request[20] == 1 && request[21] == 3 && dsu_u32(request + 12) == 0x12345678);
    assert(dsu_u32(request + 8) == dsu_crc(request, sizeof request));
    uint8_t p[101]; frame(p); DSUInput input;
    assert(dsu_decode(p, 100, 2, &input) && input.connected && input.buttons == (1u << 16));
    assert(input.axes[0] == 0 && input.axes[1] == 128 && input.axes[2] == 255);
    assert(input.acc[0] == .25 && input.acc[1] == -.5 && input.acc[2] == -1);
    assert(fabs(input.gyro[0] - 3.141592653589793) < 1e-6 && fabs(input.gyro[1] - .7853981633974483) < 1e-6 && fabs(input.gyro[2] - 1.5707963267948966) < 1e-6);
    assert(!dsu_decode(p, 99, 2, &input) && !dsu_decode(p, 100, 0, &input));
    p[37] ^= 1; assert(!dsu_decode(p, 100, 2, &input)); p[37] ^= 1;
    p[4] = 0; seal(p, 100); assert(!dsu_decode(p, 100, 2, &input)); frame(p);
    p[6] = 85; seal(p, 100); assert(!dsu_decode(p, 100, 2, &input)); frame(p);
    p[100] = 0xa5; assert(dsu_decode(p, 101, 2, &input)); // unadvertised UDP padding is ignored
    p[6] = 85; seal(p, 101); assert(dsu_decode(p, 101, 2, &input)); frame(p);
    p[39] = 0;
    const unsigned first[] = {12, 15, 14, 13, 10, 5, 4, 11}, second[] = {3, 1, 0, 2, 7, 6, 9, 8};
    for (int i = 0; i < 8; i++) {
        p[36] = 128 >> i; p[37] = 0; seal(p, 100); assert(dsu_decode(p, 100, 2, &input) && input.buttons == (1u << first[i]));
        p[36] = 0; p[37] = 128 >> i; seal(p, 100); assert(dsu_decode(p, 100, 2, &input) && input.buttons == (1u << second[i]));
    }
    put_float(p + 88, NAN); seal(p, 100); assert(!dsu_decode(p, 100, 2, &input));
    put_float(p + 88, INFINITY); seal(p, 100); assert(!dsu_decode(p, 100, 2, &input));
    p[21] = p[31] = 0; seal(p, 100); assert(dsu_decode(p, 100, 2, &input) && !input.connected && !input.buttons);
    assert(dsu_sequence_after(0, UINT32_MAX) && !dsu_sequence_after(1, 1) && !dsu_sequence_after(0, 1));
    DSUClock clock = {0}; uint64_t stamp;
    const uint64_t remote = 1700000000000000ull, now = 5000000000ull;
    assert(dsu_sample_time(&clock, remote, now, &stamp) == 2 && stamp == now);
    assert(dsu_sample_time(&clock, remote, now + 1000000, &stamp) == 0);
    assert(dsu_sample_time(&clock, remote + 10000, now + 40000000, &stamp) == 1 && stamp == now + 10000000);
    assert(dsu_sample_time(&clock, remote + 20000, now + 40000000, &stamp) == 1 && stamp == now + 20000000); // batched receive keeps sample spacing
    assert(dsu_sample_time(&clock, remote + 30000, now + 400000000, &stamp) == 0); // backlog
    assert(dsu_sample_time(&clock, remote - 1, now + 410000000, &stamp) == 2 && stamp == now + 410000000);
    assert(dsu_sample_time(&clock, remote + 9999999, now + 420000000, &stamp) == 2);
    puts("DSU protocol/configuration/clock tests passed");
    return 0;
}
