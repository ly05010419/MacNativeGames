// nn::hid vibration is persistent device state: an unwritten motor keeps its value until stopped.
// No game-specific timing, strength compensation or save-file access belongs here.
#include "hle_haptics.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

_Static_assert(sizeof(dm_vibration) == 16, "VibrationValue");
static const dm_vibration idle = {0, 160, 0, 320};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static dm_haptic_state states[DM_PLAYERS];
static void (*output)(dm_haptic_state);
static uint32_t available;
static unsigned suspended;
static int enabled;

static int valid(int p) { return p >= 0 && p < DM_PLAYERS; }
static void clear_locked(int p) {
    states[p].player = p; states[p].epoch++;
    states[p].motor[0] = states[p].motor[1] = idle;
    if (output) output(states[p]);
}
void dm_haptics_init(void (*sink)(dm_haptic_state), int on) {
    pthread_mutex_lock(&lock);
    output = sink; enabled = on; available = 0; suspended = 0;
    for (int p = 0; p < DM_PLAYERS; p++) clear_locked(p);
    pthread_mutex_unlock(&lock);
}
void dm_haptics_route(int p, int on) {
    if (!valid(p)) return;
    pthread_mutex_lock(&lock);
    if (on) available |= 1u << p; else available &= ~(1u << p);
    clear_locked(p);
    pthread_mutex_unlock(&lock);
}
void dm_haptics_clear(int p) {
    if (!valid(p)) return;
    pthread_mutex_lock(&lock); clear_locked(p); pthread_mutex_unlock(&lock);
}
uint64_t dm_haptics_epoch(int p) {
    pthread_mutex_lock(&lock);
    uint64_t e = valid(p) ? states[p].epoch : 0;
    pthread_mutex_unlock(&lock); return e;
}
int dm_haptics_enabled(void) {
    pthread_mutex_lock(&lock);
    int on = enabled && output != NULL;
    pthread_mutex_unlock(&lock); return on;
}
uint32_t dm_haptics_available(void) {
    pthread_mutex_lock(&lock);
    uint32_t m = enabled && output ? available : 0;
    pthread_mutex_unlock(&lock); return m;
}
void dm_haptics_suspend(unsigned reason, int on) {
    pthread_mutex_lock(&lock);
    unsigned before = suspended;
    if (on) suspended |= reason; else suspended &= ~reason;
    if (before != suspended) for (int p = 0; p < DM_PLAYERS; p++) clear_locked(p);
    pthread_mutex_unlock(&lock);
}
static float amplitude(float x) { return isfinite(x) ? fmaxf(0, fminf(1, x)) : 0; }
static float frequency(float x, float fallback) { return isfinite(x) && x > 0 ? fmaxf(40, fminf(1250, x)) : fallback; }
void dm_haptics_send(const dm_vibration_update *u, int n) {
    if (!u || n <= 0) return;
    pthread_mutex_lock(&lock);
    if (!enabled || !output || suspended) { pthread_mutex_unlock(&lock); return; }
    uint32_t changed = 0;
    for (int i = 0; i < n; i++) {
        int p = u[i].player, m = u[i].motor;
        if (!valid(p) || m < 0 || m > 1 || !(available >> p & 1)) continue;
        dm_vibration v = u[i].value;
        v.amplitude_low = amplitude(v.amplitude_low); v.amplitude_high = amplitude(v.amplitude_high);
        v.frequency_low = frequency(v.frequency_low, 160); v.frequency_high = frequency(v.frequency_high, 320);
        if (!memcmp(&v, &states[p].motor[m], sizeof v)) continue;
        states[p].motor[m] = v; changed |= 1u << p;
    }
    for (int p = 0; p < DM_PLAYERS; p++) if (changed >> p & 1) output(states[p]);
    pthread_mutex_unlock(&lock);
}
dm_vibration dm_haptics_actual(int p, int m) {
    pthread_mutex_lock(&lock);
    dm_vibration v = enabled && !suspended && valid(p) && m >= 0 && m < 2 && (available >> p & 1) ? states[p].motor[m] : idle;
    pthread_mutex_unlock(&lock); return v;
}
dm_haptic_level dm_haptics_level(const dm_vibration *v, int n, float gain) {
    float power = 0, weighted = 0;
    for (int i = 0; i < n; i++) {
        float lo = amplitude(v[i].amplitude_low), hi = amplitude(v[i].amplitude_high);
        lo *= lo; hi *= hi; power += lo + hi;
        weighted += lo * logf(frequency(v[i].frequency_low, 160) / 40) + hi * logf(frequency(v[i].frequency_high, 320) / 40);
    }
    if (!isfinite(gain) || gain < 0) gain = 0;
    return (dm_haptic_level){fminf(1, sqrtf(power) * gain), power > 0 ? fminf(1, weighted / (power * logf(1250.0f / 40))) : 0};
}
