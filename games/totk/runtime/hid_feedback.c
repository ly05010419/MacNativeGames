/* Controller rumble and motion on the game's side (2026-10-17): the hid functions TOTK uses for HD rumble and for the
   six-axis sensor (motion aiming).
   Chosen at start, because the game asks for its vibration and six-axis handles only when a controller's style is set
   up (main+0x29b45cc, from the per-frame controller update main+0x29b442c), so they cannot appear later:
     TOTK_RUMBLE    the controller has its two vibration devices (left, right); what the game plays goes to the Mac
                    controller's haptics (runtime/gamepad_feedback.m);
     TOTK_GYRO      the controller has its six-axis sensor; the game gets the Mac controller's motion.
   Both are on by default (setting_on below); =0 turns one off, as does rumble.txt / gyro.txt saying off, and the game then
   does not get that device, exactly as before (no handles for it, its code below is not called).
   While the game runs, "flag rumble 0|1" / "flag gyro 0|1" mute and unmute them (muted motion = a controller lying still).
   The handles are the SDK's own: nn::hid::utility computes them without the hid service (byte 0 = style index, 3 full-key,
   4 handheld; byte 1 = controller id; byte 2 = device index: vibration 0 left / 1 right, six-axis 2). Everything under
   them (nn::hid::detail, which the SDK calls through its PLT, including the batches VibrationTarget sends through
   detail::EnqueueVibrationValue -> detail::SendVibrationValues) is answered here instead of by the hid service.
   Layouts: VibrationValue {amplitude low, frequency low, amplitude high, frequency high} (0..1 and Hz; neutral 0/160/0/320,
   sdk+0x4a2df8); VibrationDeviceInfo {type, position} (8 bytes, sdk+0x49da78); SixAxisSensorState 0x60 bytes. */
#include "runtime.h"
#include <TargetConditionals.h>
#include "feedback_settings.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TARGET_OS_IOS
/* ios/Sources/phone_haptics.m: the game's vibration on the iPhone's own Taptic Engine while the touch controls are used. */
void runtime_phone_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high);
#endif
typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct { uint32_t type, position; } VibrationDeviceInfo;       /* type 1 = linear resonant actuator; position 1 left, 2 right */
typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9]; uint32_t attributes, reserved; } SixAxisState;
_Static_assert(sizeof(VibrationValue) == 16 && sizeof(VibrationDeviceInfo) == 8 && sizeof(SixAxisState) == 0x60, "hid ABI");
enum { STYLE_FULL_KEY = 3, STYLE_HANDHELD = 4, SIX_AXIS_CONNECTED = 1 };

/* On unless =0 (2026-10-17: default on, the user's request), or, without the variable, unless rumble.txt / gyro.txt next to
   controls.txt says off (runtime/feedback_settings.h). An agent's test session (TOTK_SLOT_OWNER claude / codex) leaves
   them off unless set, so a test does not rumble the user's controller and tests stay as measured before. The files are
   read once, the first time it is asked, so a run keeps one answer even if a file changes while the game plays; the
   environment is read on every call, as before. */
static char rumbleText[16], gyroText[16];
static bool rumbleFile, gyroFile;
static void read_files(void) {
    rumbleFile = runtime_setting_word("rumble.txt", rumbleText, sizeof rumbleText);
    gyroFile = runtime_setting_word("gyro.txt", gyroText, sizeof gyroText);
}
static pthread_once_t filesRead = PTHREAD_ONCE_INIT;
static bool setting_on(const char *env, const char *file, const char *text, _Atomic int *logged) {
    int from, on = feedback_switch(getenv(env), getenv("TOTK_SLOT_OWNER"), text, &from);
    if (!atomic_exchange(logged, 1)) {
        if (from == FEEDBACK_FROM_BAD_FILE) fprintf(stderr, "[hid] %s: not on or off; on\n", file);
        else if (from == FEEDBACK_FROM_FILE && !on) fprintf(stderr, "[hid] %s: off\n", file);
    }
    return on;
}
static _Atomic int rumbleLogged, gyroLogged;
int runtime_rumble_enabled(void) {
    pthread_once(&filesRead, read_files);
    return setting_on("TOTK_RUMBLE", "rumble.txt", rumbleFile ? rumbleText : NULL, &rumbleLogged);
}
int runtime_gyro_enabled(void) {
    pthread_once(&filesRead, read_files);
    return setting_on("TOTK_GYRO", "gyro.txt", gyroFile ? gyroText : NULL, &gyroLogged);
}
static _Atomic int rumble_muted, gyro_muted;
static pthread_mutex_t lastLock = PTHREAD_MUTEX_INITIALIZER;
static VibrationValue last[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};   /* lastLock */
static _Atomic uint64_t values_in, reads;

static void note_once(_Atomic int *flag, const char *text, uint32_t handle) {
    if (!atomic_exchange(flag, 1)) fprintf(stderr, "[hid] %s (handle %#x)\n", text, handle);
}
/* The device a handle names, or -1 (a style this runtime never reports, logged once). */
static int vibration_side(const uint32_t *h) {
    if (!h) runtime_fail("vibration device handle pointer");
    unsigned style = *h & 0xff, index = (*h >> 16) & 0xff;
    if ((style == STYLE_FULL_KEY || style == STYLE_HANDHELD) && index < 2) return (int)index;
    static _Atomic int warned; note_once(&warned, "vibration call with a handle this controller does not have; ignored", *h);
    return -1;
}
static void play(int side, const VibrationValue *v) {
    if (!v) runtime_fail("vibration value pointer");
    if (side < 0) return;
    VibrationValue c = *v;
    if (!isfinite(c.amp_low) || !isfinite(c.amp_high) || !isfinite(c.freq_low) || !isfinite(c.freq_high)) c = (VibrationValue){0, 160, 0, 320};
    pthread_mutex_lock(&lastLock); last[side] = c; pthread_mutex_unlock(&lastLock); atomic_fetch_add(&values_in, 1);
    static _Atomic int first; if (!atomic_exchange(&first, 1)) fprintf(stderr, "[hid] first vibration value from the game: %s %.2f@%.0f Hz %.2f@%.0f Hz\n",
                                                                         side ? "right" : "left", c.amp_low, c.freq_low, c.amp_high, c.freq_high);
    if (atomic_load(&rumble_muted)) c.amp_low = c.amp_high = 0;
    runtime_rumble_set((unsigned)side, c.amp_low, c.freq_low, c.amp_high, c.freq_high);
#if TARGET_OS_IOS
    runtime_phone_rumble_set((unsigned)side, c.amp_low, c.freq_low, c.amp_high, c.freq_high);
#endif
}
static uint32_t vib_init(const uint32_t *h) {
    int side = vibration_side(h); static _Atomic int once;
    if (side >= 0) note_once(&once, "the game initialized its vibration devices", *h);
    return 0;
}
static uint32_t vib_send(const uint32_t *h, const VibrationValue *v) { play(vibration_side(h), v); return 0; }
static uint32_t vib_send_many(const uint32_t *h, const VibrationValue *v, int n) {
    if (n < 0 || (n && (!h || !v))) runtime_fail("vibration batch");
    for (int i = 0; i < n; i++) play(vibration_side(&h[i]), &v[i]);
    return 0;
}
static uint32_t vib_actual(VibrationValue *out, const uint32_t *h) {
    if (!out) runtime_fail("vibration value output pointer");
    int side = vibration_side(h); *out = (VibrationValue){0, 160, 0, 320};
    if (side >= 0) { pthread_mutex_lock(&lastLock); *out = last[side]; pthread_mutex_unlock(&lastLock); }
    return 0;
}
static uint32_t vib_info(VibrationDeviceInfo *out, const uint32_t *h) {
    if (!out) runtime_fail("vibration device info pointer");
    int side = vibration_side(h); *out = (VibrationDeviceInfo){side >= 0 ? 1u : 0u, side >= 0 ? (uint32_t)side + 1 : 0}; return 0;
}
static uint32_t vib_mounted(bool *out, const uint32_t *h) { if (!out) runtime_fail("vibration mounted pointer"); *out = vibration_side(h) >= 0; return 0; }
static uint32_t vib_permitted(bool *out) { if (!out) runtime_fail("vibration permitted pointer"); *out = true; return 0; }

static void six_axis_check(const uint32_t *h) {
    if (!h) runtime_fail("six-axis handle pointer");
    unsigned style = *h & 0xff;
    if (style != STYLE_FULL_KEY && style != STYLE_HANDHELD) { static _Atomic int warned; note_once(&warned, "six-axis call with a handle this controller does not have", *h); }
}
static uint32_t six_start(const uint32_t *h) {
    six_axis_check(h); static _Atomic int once; note_once(&once, "the game started the six-axis sensor", *h);
    runtime_motion_start(); return 0;
}
static uint32_t six_stop(const uint32_t *h) { six_axis_check(h); return 0; }
/* Newest first, like the hid service's ring. No motion (no controller with sensors, or muted): one state of a controller
   lying still, not marked connected. */
static uint32_t six_states(int *out, SixAxisState *s, int max, const uint32_t *h) {
    if (!out || (max > 0 && !s)) runtime_fail("six-axis state output");
    six_axis_check(h); atomic_fetch_add(&reads, 1);
    if (max <= 0) { *out = 0; return 0; }
    RuntimeMotionState m[RUNTIME_MOTION_MAX];
    int n = atomic_load(&gyro_muted) ? 0 : runtime_motion_read(m, max < RUNTIME_MOTION_MAX ? max : RUNTIME_MOTION_MAX);
    if (n <= 0) {
        s[0] = (SixAxisState){.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
        *out = 1; return 0;
    }
    for (int i = 0; i < n; i++) {
        SixAxisState *d = &s[i]; *d = (SixAxisState){.delta_ns = (int64_t)m[i].delta_ns, .sampling = (int64_t)m[i].sampling, .attributes = SIX_AXIS_CONNECTED};
        memcpy(d->acc, m[i].acc, sizeof d->acc); memcpy(d->gyro, m[i].gyro, sizeof d->gyro);
        memcpy(d->angle, m[i].angle, sizeof d->angle); memcpy(d->dir, m[i].dir, sizeof d->dir);
    }
    *out = n; return 0;
}
static uint32_t six_state(SixAxisState *s, const uint32_t *h) { int n; return six_states(&n, s, 1, h); }
static int no_handles(void *out, int max, const uint32_t *id, uint32_t style) { (void)out; (void)max; (void)id; (void)style; return 0; }

/* "flag rumble 0|1", "flag gyro 0|1": 1 if the name was one of these. */
int runtime_hid_feedback_flag(const char *name, int v) {
    _Atomic int *muted = !strcmp(name, "rumble") ? &rumble_muted : !strcmp(name, "gyro") ? &gyro_muted : NULL;
    if (!muted) return 0;
    if (v != 0 && v != 1) runtime_fail("flag rumble / gyro takes 0 or 1");
    int on = muted == &rumble_muted ? runtime_rumble_enabled() : runtime_gyro_enabled();
    atomic_store(muted, !v);
    if (!v && muted == &rumble_muted) { runtime_rumble_set(0, 0, 160, 0, 320); runtime_rumble_set(1, 0, 160, 0, 320); }
    fprintf(stderr, "[hid] %s %s%s\n", name, v ? "on" : "muted", on ? "" : " (no effect: the game has no such device in this run; start it with TOTK_RUMBLE=1 / TOTK_GYRO=1, or without \"rumble\" / \"gyro\" saying off in settings.json)");
    return 1;
}
/* The settings panel (key 1, runtime/settings_panel.h). The variable or an agent's test session decides first, as above; the
   row then shows that value, greyed out. Otherwise the switch shows the choice in force: until the player picks, whether
   the game has the device and it is not muted. A pick is saved in rumble.txt / gyro.txt for the next launch and, when the
   game has the device in this run, mutes or unmutes it at once, as "flag rumble|gyro 0|1" does. A device the run started
   without comes at the next launch: the game asked for its devices at the start. */
static _Atomic int rumbleChoice = -1, gyroChoice = -1;   /* -1: no pick in this run */
int runtime_feedback_locked(int gyro) {
    const char *o = getenv("TOTK_SLOT_OWNER");
    return getenv(gyro ? "TOTK_GYRO" : "TOTK_RUMBLE") || (o && (!strcmp(o, "claude") || !strcmp(o, "codex")));
}
int runtime_feedback_present(int gyro) { return gyro ? runtime_gyro_enabled() : runtime_rumble_enabled(); }
int runtime_feedback_choice(int gyro) {
    int c = atomic_load(gyro ? &gyroChoice : &rumbleChoice);
    return c >= 0 ? c : runtime_feedback_present(gyro) && !atomic_load(gyro ? &gyro_muted : &rumble_muted);
}
void runtime_feedback_choose(int gyro, int on) {
    if (runtime_feedback_locked(gyro)) return;
    on = !!on;
    atomic_store(gyro ? &gyroChoice : &rumbleChoice, on);
    runtime_setting_save(gyro ? "gyro.txt" : "rumble.txt", on ? "on" : "off", gyro ? "gyro setting" : "rumble setting");
    if (runtime_feedback_present(gyro)) runtime_hid_feedback_flag(gyro ? "gyro" : "rumble", on);
    else fprintf(stderr, "[hid] %s %s from the next launch (saved)\n", gyro ? "gyro" : "rumble", on ? "on" : "off");
}
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads) { *values = atomic_load(&values_in); *motion_reads = atomic_load(&reads); }

void *runtime_hid_feedback_override(const char *name) {
    const bool rumble = runtime_rumble_enabled(), gyro = runtime_gyro_enabled();
    if (!strcmp(name, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"))
        return rumble ? NULL : (void *)no_handles;   /* on: the SDK's own (nn::hid::utility) */
    if (!strcmp(name, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"))
        return gyro ? NULL : (void *)no_handles;
    static const struct { const char *name; void *fn; bool gyro; } entries[] = {
        {"_ZN2nn3hid6detail25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", (void *)vib_init, false},
        {"_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", (void *)vib_send, false},
        {"_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", (void *)vib_send_many, false},
        {"_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", (void *)vib_actual, false},
        {"_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", (void *)vib_info, false},
        {"_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE", (void *)vib_mounted, false},
        {"_ZN2nn3hid6detail20IsVibrationPermittedEPb", (void *)vib_permitted, false},
        {"_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", (void *)six_start, true},
        {"_ZN2nn3hid6detail17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", (void *)six_stop, true},
        {"_ZN2nn3hid6detail22GetSixAxisSensorStatesEPiPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", (void *)six_states, true},
        {"_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", (void *)six_state, true}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return (entries[i].gyro ? gyro : rumble) ? entries[i].fn : NULL;
    return NULL;
}
