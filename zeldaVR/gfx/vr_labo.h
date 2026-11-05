/* Breath of the Wild's native Labo renderer. The mode is opt-in on Mac; the
 * iOS runtime and Mac-side iOS export always report it unavailable. */
#pragma once
#include <stdint.h>
#include <stddef.h>

int vr_labo_enabled(void);
/* Game option policy, separate from our renderer/sensor. Always unavailable,
 * including in our VR and in explicitly enabled research sessions. */
unsigned char vr_labo_option_permitted(unsigned char show_error);
int vr_labo_active(void);
void vr_labo_begin(void);
void vr_labo_end(void);

/* SDK 7 SevenSixAxisSensorState, verified against BotW 1.6.0's reader
 * (main+0xe3c234..0xe3c278) and libnx's hid.h. No SDK implementation is copied. */
typedef struct {
    uint64_t timestamp, sampling_number, reserved;
    float acceleration[3], angular_velocity[3], orientation[4];
} vr_labo_sensor_state;
_Static_assert(sizeof(vr_labo_sensor_state) == 0x40, "SevenSixAxisSensorState ABI");
uint32_t vr_labo_sensor_init(void *work, unsigned long size);
uint32_t vr_labo_sensor_finalize(void);
uint32_t vr_labo_sensor_start(void);
uint32_t vr_labo_sensor_stop(void);
void vr_labo_sensor_reset(void);
int vr_labo_sensor_states(void *out, int count);
void vr_labo_stats(char *out, size_t size);

/* Complete shader-code SHA256, excluding the translator/settings suffix. */
int vr_labo_shader_pair(const char *vertex, const char *fragment);
int vr_labo_lens_pair(const char *vertex, const char *fragment);
int vr_labo_plain_mesh(const char *vertex);
int vr_native_ui_vertex(const char *vertex);

int vr_native_ui_text_vertex(const char *vertex);
