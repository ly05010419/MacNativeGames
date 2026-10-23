// runtime/hid_imu.h: the six-axis sensor in the input reports of a pad GameController gives no motion (8BitDo Ultimate 2 Wireless).
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "hid_imu.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }
static void put16(uint8_t *r, int at, int v) { r[at] = (uint8_t)(v & 255); r[at + 1] = (uint8_t)((v >> 8) & 255); }

int main(void) {
    const HidImuProfile *pr = hid_imu_find(0x2dc8, 0x6012);
    check(pr != NULL, "the 8BitDo Ultimate 2 Wireless has a profile");
    check(!hid_imu_find(0x057e, 0x2009) && !hid_imu_find(0x2dc8, 0x6009) && !hid_imu_find(0, 0), "other controllers have none");
    if (!pr) return 1;
    const double deg = M_PI / 180, dps_max = 2000 * deg;
    double acc[3], gyro[3];

    /* reports captured from the pad over Bluetooth (2026-10-23): report id 1, 34 bytes, the sensor from byte 15 */
    static const uint8_t real[34] = {0x01, 0x0f, 0x7f, 0x7f, 0x7f, 0x7f, 0, 0, 0, 0, 0, 0, 0x12, 0x60, 0x59,
                                     0x6e, 0x0b, 0xc3, 0xfc, 0x28, 0x0b, 0xee, 0xff, 0x11, 0x00, 0xd6, 0xff, 0, 0, 0, 0, 0, 0, 0};
    check(hid_imu_decode(pr, 1, real, sizeof real, acc, gyro), "a real report decodes");
    /* raw acceleration (2926, -829, 2856) counts of 4096 = 1 G, raw rotation (-18, 17, -42) */
    check(near(acc[0], -829 / 4096.0, 1e-9) && near(acc[1], -2926 / 4096.0, 1e-9) && near(acc[2], -2856 / 4096.0, 1e-9), "real report: acceleration in GCMotion's frame, as gravity");
    check(near(sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]), 1.0, 0.03), "real report: about 1 G held still");
    check(near(gyro[0], -17 / 16.3835 * deg, 1e-6) && near(gyro[1], -18 / 16.3835 * deg, 1e-6) && near(gyro[2], -42 / 16.3835 * deg, 1e-6), "real report: rotation in rad/s");

    /* lying face up: z = +1 G on the sensor is -1 G as gravity in GCMotion's frame */
    uint8_t r[34] = {1};
    put16(r, 15 + 4, 4096);
    check(hid_imu_decode(pr, 1, r, sizeof r, acc, gyro) && near(acc[0], 0, 1e-9) && near(acc[1], 0, 1e-9) && near(acc[2], -1, 1e-9), "face up reads z = -1 G");
    check(near(gyro[0], 0, 1e-12) && near(gyro[1], 0, 1e-12) && near(gyro[2], 0, 1e-12), "and no rotation");

    /* the three gyro axes land where the Switch Pro Controller's HID path puts them: raw y -> -x, raw x -> y, raw z -> z */
    memset(r, 0, sizeof r); r[0] = 1; put16(r, 15 + 6, 32767);
    hid_imu_decode(pr, 1, r, sizeof r, acc, gyro);
    check(near(gyro[0], 0, 1e-12) && near(gyro[1], dps_max, 1e-9) && near(gyro[2], 0, 1e-12), "raw gyro x full scale = +2000 deg/s on y");
    memset(r, 0, sizeof r); r[0] = 1; put16(r, 15 + 8, 32767);
    hid_imu_decode(pr, 1, r, sizeof r, acc, gyro);
    check(near(gyro[0], -dps_max, 1e-9) && near(gyro[1], 0, 1e-12) && near(gyro[2], 0, 1e-12), "raw gyro y full scale = -2000 deg/s on x");
    memset(r, 0, sizeof r); r[0] = 1; put16(r, 15 + 10, -32768);
    hid_imu_decode(pr, 1, r, sizeof r, acc, gyro);
    check(near(gyro[2], -32768 / 32767.0 * dps_max, 1e-9) && near(gyro[0], 0, 1e-12) && near(gyro[1], 0, 1e-12), "raw gyro z negative full scale on z");

    /* acceleration axes: raw x -> -y, raw y -> x */
    memset(r, 0, sizeof r); r[0] = 1; put16(r, 15, 4096);
    hid_imu_decode(pr, 1, r, sizeof r, acc, gyro);
    check(near(acc[0], 0, 1e-9) && near(acc[1], -1, 1e-9) && near(acc[2], 0, 1e-9), "raw acceleration x = +1 G is -1 G on y");
    memset(r, 0, sizeof r); r[0] = 1; put16(r, 17, 4096);
    hid_imu_decode(pr, 1, r, sizeof r, acc, gyro);
    check(near(acc[0], 1, 1e-9) && near(acc[1], 0, 1e-9) && near(acc[2], 0, 1e-9), "raw acceleration y = +1 G is +1 G on x");

    /* reports that do not carry the sensor */
    acc[0] = acc[1] = acc[2] = gyro[0] = gyro[1] = gyro[2] = 7;
    check(!hid_imu_decode(pr, 2, real, sizeof real, acc, gyro), "another report id is skipped");
    check(!hid_imu_decode(pr, 1, real, 12, acc, gyro), "firmware 1.02's 12-byte report is skipped");
    check(!hid_imu_decode(pr, 1, real, 33, acc, gyro), "a report one byte short is skipped");
    check(acc[0] == 7 && gyro[2] == 7, "and nothing is written for them");

    /* negative control: reading from the wrong byte gives other numbers than the real report's */
    HidImuProfile wrong = *pr; wrong.offset = 14;
    double acc2[3], gyro2[3];
    hid_imu_decode(&wrong, 1, real, sizeof real, acc2, gyro2);
    hid_imu_decode(pr, 1, real, sizeof real, acc, gyro);
    check(!near(acc[1], acc2[1], 0.01), "negative control: a one-byte offset error is caught");


    /* the sample's time: the report's kernel receive time, not the callback's */
    uint64_t last = 0;
    check(hid_imu_time(1000000000ull, 1005000000ull, &last) == 1000000000ull, "a plausible kernel time is used");
    check(hid_imu_time(1015800000ull, 1016000000ull, &last) == 1015800000ull && last == 1015800000ull, "and kept as the last time");
    /* a busy main thread: four reports 15 ms apart are delivered in one go, 60 ms late, all with the same 'now' */
    uint64_t t0 = 2000000000ull, got[4], lastB = 0;
    for (int i = 0; i < 4; i++) got[i] = hid_imu_time(t0 + i * 15000000ull, t0 + 60000000ull + i * 100000ull, &lastB);
    check(got[1] - got[0] == 15000000ull && got[2] - got[1] == 15000000ull && got[3] - got[2] == 15000000ull, "bunched callbacks keep the reports' own 15 ms spacing");
    last = 5000000000ull;
    check(hid_imu_time(5000000000ull, 5001000000ull, &last) == 5000000001ull, "the same stamp twice is nudged on (never equal or backwards)");
    check(hid_imu_time(4990000000ull, 5002000000ull, &last) == 5000000002ull, "an older stamp is nudged past the last one too");
    last = 0;
    check(hid_imu_time(0, 7000000000ull, &last) == 7000000000ull, "no kernel time: now");
    last = 0;
    check(hid_imu_time(9000000000ull, 7000000000ull, &last) == 7000000000ull, "a stamp from the future: now");
    last = 0;
    check(hid_imu_time(1000000000ull, 7000000000ull, &last) == 7000000000ull, "a stamp over a second old: now");
    last = 0;
    check(hid_imu_time(7003000000ull, 7000000000ull, &last) == 7003000000ull, "a stamp 3 ms ahead of now is still used");

    printf("{\"status\": \"%s\", \"checked\": %d, \"failures\": %d}\n", failures ? "fail" : "pass", checked, failures);
    return failures ? 1 : 0;
}
