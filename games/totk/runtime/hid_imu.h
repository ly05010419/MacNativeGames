/* The six-axis sensor inside the input reports of a pad GameController gives no motion (2026-10-23; used by
   runtime/gamepad_feedback.m, covered by tests/hid_imu/probe.c).
   The 8BitDo Ultimate 2 Wireless (2dc8:6012, Bluetooth, firmware 1.03 and later) is adopted by GameController as a plain
   extended gamepad (productCategory "HID") with motion == nil, so the game found no gyro. Its 34-byte input reports (id 1)
   carry the sensor, though: int16 little endian from byte 15 (the report id is byte 0), acceleration x, y, z, then
   rotation x, y, z; 4096 = 1 G and +-2000 deg/s over 32767 (SDL's SDL_hidapi_8bitdo.c). Measured on this pad over
   Bluetooth: about 63 reports a second; lying face up z = +1 G; the gyro at rest within 0.5 deg/s of zero with 0.1 deg/s
   noise, so no calibration is read; a roll moves x, a pitch y, a turn about the vertical z. Those are the Switch Pro
   Controller's raw axes (x toward the top edge, y to the left, z out of the face), so the frame change is the one
   gamepad_feedback.m applies to a Switch Pro's HID reports. Firmware 1.02 sends 12-byte reports without the sensor.
   A line in the table is one more controller of this kind. */
#ifndef HID_IMU_H
#define HID_IMU_H
#include <math.h>
#include <stdint.h>

typedef struct { uint16_t vendor, product; uint8_t reportId, length, offset; double countsPerG, countsPerDegS; } HidImuProfile;

static const HidImuProfile hid_imu_profiles[] = { {0x2dc8, 0x6012, 0x01, 34, 15, 4096.0, 32767.0 / 2000.0} };
enum { HID_IMU_PROFILES = sizeof hid_imu_profiles / sizeof *hid_imu_profiles };

static inline const HidImuProfile *hid_imu_find(int vendor, int product) {
    for (int i = 0; i < HID_IMU_PROFILES; i++)
        if (hid_imu_profiles[i].vendor == vendor && hid_imu_profiles[i].product == product) return &hid_imu_profiles[i];
    return 0;
}

/* One input report (id, bytes with the id as byte 0) to GCMotion's frame: acceleration in G as gravity (face up z = -1 G)
   and rotation in rad/s (x right, y toward the top edge, z out of the face). 0 if it is not the report that carries the
   sensor (another id) or is too short for it. */
static inline int hid_imu_decode(const HidImuProfile *pr, uint32_t id, const uint8_t *p, long len, double acc[3], double gyro[3]) {
    if (id != pr->reportId || len < pr->length) return 0;
    const uint8_t *s = p + pr->offset;
    double raw[6];
    for (int i = 0; i < 6; i++) {
        double v = (int16_t)(s[2 * i] | s[2 * i + 1] << 8);
        raw[i] = i < 3 ? v / pr->countsPerG : v / pr->countsPerDegS * (M_PI / 180);
    }
    acc[0] = raw[1]; acc[1] = -raw[0]; acc[2] = -raw[2];
    gyro[0] = -raw[4]; gyro[1] = raw[3]; gyro[2] = raw[5];
    return 1;
}
#endif
