#include "vr_labo.h"
#include "totk_vr_link.h"
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

_Static_assert(offsetof(vr_labo_sensor_state, orientation) == 0x30, "Labo quaternion ABI");

unsigned char vr_labo_option_permitted(unsigned char show_error) {
    (void)show_error;
    return 0;
}

int vr_labo_enabled(void) {
#if defined(GAME_BOTW) && TARGET_OS_OSX
    if (getenv("TOTK_EXPORT_IOS")) return 0;
    const char *control = getenv("TOTK_VR_CONTROL");
    return control && *control;
#else
    return 0;
#endif
}

static struct {
    pthread_mutex_t mu;
    int active, initialized, running;
    uint64_t origin, last_timestamp, sample, delivered;
} sensor = { .mu = PTHREAD_MUTEX_INITIALIZER };

int vr_labo_active(void) {
    pthread_mutex_lock(&sensor.mu);
    int active = sensor.active && vr_labo_enabled();
    pthread_mutex_unlock(&sensor.mu);
    return active;
}
void vr_labo_begin(void) {
    pthread_mutex_lock(&sensor.mu); sensor.active = vr_labo_enabled(); pthread_mutex_unlock(&sensor.mu);
}
void vr_labo_end(void) {
    pthread_mutex_lock(&sensor.mu); sensor.active = 0; pthread_mutex_unlock(&sensor.mu);
}
uint32_t vr_labo_sensor_init(void *work, unsigned long size) {
    (void)work; (void)size; /* The native host needs no Horizon transfer memory. */
    pthread_mutex_lock(&sensor.mu); sensor.initialized = 1; pthread_mutex_unlock(&sensor.mu);
    return 0;
}
uint32_t vr_labo_sensor_finalize(void) {
    pthread_mutex_lock(&sensor.mu);
    sensor.initialized = sensor.running = 0;
    sensor.origin = sensor.last_timestamp = sensor.sample = 0;
    pthread_mutex_unlock(&sensor.mu);
    return 0;
}
uint32_t vr_labo_sensor_start(void) {
    pthread_mutex_lock(&sensor.mu); sensor.running = sensor.initialized && vr_labo_enabled(); pthread_mutex_unlock(&sensor.mu);
    return 0;
}
uint32_t vr_labo_sensor_stop(void) {
    pthread_mutex_lock(&sensor.mu); sensor.running = 0; pthread_mutex_unlock(&sensor.mu);
    return 0;
}
void vr_labo_sensor_reset(void) {
    pthread_mutex_lock(&sensor.mu); sensor.origin = sensor.last_timestamp = sensor.sample = 0; pthread_mutex_unlock(&sensor.mu);
}
int vr_labo_sensor_states(void *out, int count) {
    if (!out || count <= 0 || !vr_labo_enabled()) return 0;
    vr_pose pose;
    if (!vr_link_alive() || !vr_link_pose(&pose) || !pose.ts) return 0;
    double norm = 0;
    for (int i = 0; i < 4; i++) { if (!isfinite(pose.q[i])) return 0; norm += (double)pose.q[i] * pose.q[i]; }
    if (!(norm > 0.5 && norm < 1.5)) return 0;
    pthread_mutex_lock(&sensor.mu);
    if (!sensor.initialized || !sensor.running || !sensor.active) { pthread_mutex_unlock(&sensor.mu); return 0; }
    /* A reconnect may start a new timestamp epoch; never underflow the elapsed time. */
    if (!sensor.sample || pose.ts < sensor.last_timestamp) sensor.origin = pose.ts;
    if (!sensor.sample || pose.ts != sensor.last_timestamp) sensor.sample++;
    sensor.last_timestamp = pose.ts;
    vr_labo_sensor_state state = { .timestamp = pose.ts - sensor.origin, .sampling_number = sensor.sample };
    for (int i = 0; i < 4; i++) state.orientation[i] = (float)(pose.q[i] / sqrt(norm));
    memcpy(out, &state, sizeof state); /* One real tracked sample, never fabricated history. */
    sensor.delivered++;
    pthread_mutex_unlock(&sensor.mu);
    return 1;
}
void vr_labo_stats(char *out, size_t size) {
    pthread_mutex_lock(&sensor.mu);
    snprintf(out, size, "sensor initialized %d, running %d, VR active %d, tracked states delivered %llu",
        sensor.initialized, sensor.running, sensor.active, (unsigned long long)sensor.delivered);
    pthread_mutex_unlock(&sensor.mu);
}

int vr_labo_shader_pair(const char *vertex, const char *fragment) {
    static const char vs[] = "5e9f57168d89a543a537832326b844bcba3255df655c10dc9359cb195304e897";
    static const char fs[] = "bd7ffdd435c6c3bc997653d5fb9a0cc7f2f411dd51ce88c3cafe6574c8624908";
    return vertex && fragment && strlen(vertex) >= 64 && strlen(fragment) >= 64 &&
           !memcmp(vertex, vs, 64) && !memcmp(fragment, fs, 64) &&
           (!vertex[64] || vertex[64] == '-') && (!fragment[64] || fragment[64] == '-');
}
int vr_labo_lens_pair(const char *vertex, const char *fragment) {
    static const char vs[] = "e0467470d532d62384dd46e87619cab73382a02de7d51a1522c0bd23c404245b";
    static const char fs[] = "cfa1bb2deb2e2cc1b367027250a3c03c4a48d105ecda5262a96b2c96d708918e";
    return vertex && fragment && strlen(vertex) >= 64 && strlen(fragment) >= 64 &&
           !memcmp(vertex, vs, 64) && !memcmp(fragment, fs, 64) &&
           (!vertex[64] || vertex[64] == '-') && (!fragment[64] || fragment[64] == '-');
}
/* Original UI vertex programs recorded writing both halves of the native
 * canvas in the inventory and map. Full shader identities, never size alone. */
int vr_native_ui_vertex(const char *vertex) {
    static const char *keys[] = {
        "62f5ad35ffe7b2699c002728041fb83bd2b941b19d1936a4b369eeecab6e879d",
        "35e3c8217a496bdefd3393f2a514ff1d8c54958d28a12c0e7ccaee5445e97132",
        "af24932849a17f4d5d0635e5f090bd65004f8d6eeead7e2ad42d6ff38f3c77ab",
        "0efd7d0b6c368cb1be82333ee021ec0b864b407c3e466fcd0d2ec36ae048a5a9",
        "3bd85812eda98e6b0cd7d9588176769c44f22b30f0bfe4d9b856f76e1524589c",
        "5ecc10804849bfbbdf9f99aef7390db2a4ee83ede9f4cd42f5a7053976380881",
        "46c3223d1857bf0995ce7478539cdad47a170f10ae7c5f51061e9dda83774713"
    };
    for (unsigned i=0;i<sizeof keys/sizeof *keys;i++)
        if (vertex && strlen(vertex)>=64 && !strncmp(vertex,keys[i],64) && (!vertex[64] || vertex[64]=='-')) return 1;
    return vr_native_ui_text_vertex(vertex);
}
/* Instanced glyphs use a precombined P*model in CB0 (128 bytes), but the
 * same canvas/eye UI camera and the same native-eye CB10 as the widgets. */
int vr_native_ui_text_vertex(const char *vertex) {
    static const char key[]="d80c41a52116d05812af0999ebe1f6c4351a750d80d7bb4e33f30df8dc019929";
    return vertex && strlen(vertex)>=64 && !memcmp(vertex,key,64) && (!vertex[64]||vertex[64]=='-');
}
int vr_labo_plain_mesh(const char *vertex) {
    /* This complete VS defines the UV/position mapping for several circular
       post passes (deferred composition, depth of field and tone mapping).
       Their fragments stay intact; changing only the last pass leaves a hole. */
    static const char vs[] = "1f037afbe71a66f468ae9bb31a29e6198278c99957d7cec2642c69c49cb3c13d";
    return vertex && strlen(vertex)>=64 && !memcmp(vertex,vs,64) && (!vertex[64]||vertex[64]=='-');
}
