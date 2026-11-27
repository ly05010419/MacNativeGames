/* Controller rumble and motion on the game's side (2026-10-17): the hid functions TOTK uses for HD rumble and for the
   six-axis sensor (motion aiming).
   Opt-in at start, because the game asks for its vibration and six-axis handles only when a controller's style is set
   up (main+0x29b45cc, from the per-frame controller update main+0x29b442c), so they cannot appear later:
     TOTK_RUMBLE=1  the controller has its two vibration devices (left, right); what the game plays goes to the Mac
                    controller's haptics (runtime/gamepad_feedback.m);
     TOTK_GYRO=1    the controller has its six-axis sensor; the game gets the Mac controller's motion.
   Off (the default) the game sees neither, exactly as before (no handles, nothing below is called).
   While the game runs, "flag rumble 0|1" / "flag gyro 0|1" mute and unmute them (muted motion = a controller lying still).
   The handles are the SDK's own: nn::hid::utility computes them without the hid service (byte 0 = style index, 3 full-key,
   4 handheld; byte 1 = controller id; byte 2 = device index: vibration 0 left / 1 right, six-axis 2). Everything under
   them (nn::hid::detail, which the SDK calls through its PLT, including the batches VibrationTarget sends through
   detail::EnqueueVibrationValue -> detail::SendVibrationValues) is answered here instead of by the hid service.
   Layouts: VibrationValue {amplitude low, frequency low, amplitude high, frequency high} (0..1 and Hz; neutral 0/160/0/320,
   sdk+0x4a2df8); VibrationDeviceInfo {type, position} (8 bytes, sdk+0x49da78); SixAxisSensorState 0x60 bytes. */
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { float amp_low, freq_low, amp_high, freq_high; } VibrationValue;
typedef struct { uint32_t type, position; } VibrationDeviceInfo;       /* type 1 = linear resonant actuator; position 1 left, 2 right */
typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9]; uint32_t attributes, reserved; } SixAxisState;
_Static_assert(sizeof(VibrationValue) == 16 && sizeof(VibrationDeviceInfo) == 8 && sizeof(SixAxisState) == 0x60, "hid ABI");
enum { STYLE_FULL_KEY = 3, STYLE_HANDHELD = 4, SIX_AXIS_CONNECTED = 1 };

/* On unless =0 (2026-10-17: default on, the user's request). An agent's test session (TOTK_SLOT_OWNER claude / codex) leaves
   them off unless set, so a test does not rumble the user's controller and tests stay as measured before. */
static bool env_on(const char *name) {
    const char *e = getenv(name); if (e) return strcmp(e, "0") != 0;
    const char *o = getenv("TOTK_SLOT_OWNER");
    return !(o && (!strcmp(o, "claude") || !strcmp(o, "codex")));
}
int runtime_rumble_enabled(void) { return env_on("TOTK_RUMBLE"); }
int runtime_gyro_enabled(void) { return env_on("TOTK_GYRO"); }
static _Atomic int rumble_muted, gyro_muted;
static pthread_mutex_t lastLock = PTHREAD_MUTEX_INITIALIZER;
static VibrationValue last[RUNTIME_GAMEPAD_PLAYERS][2] = {
    {{0, 160, 0, 320}, {0, 160, 0, 320}}, {{0, 160, 0, 320}, {0, 160, 0, 320}}
}; /* lastLock */
void runtime_hid_feedback_clear_player(unsigned player) {
    if (player >= RUNTIME_GAMEPAD_PLAYERS) return;
    pthread_mutex_lock(&lastLock);
    for (unsigned side = 0; side < 2; side++) last[player][side] = (VibrationValue){0, 160, 0, 320};
    pthread_mutex_unlock(&lastLock);
    /* Do not call back into the input layer: DisconnectNpad already cleared it. */
    for (unsigned side = 0; side < 2; side++) runtime_rumble_set_player(player, side, 0, 160, 0, 320);
}
static _Atomic uint64_t values_in, reads;

static void note_once(_Atomic int *flag, const char *text, uint32_t handle) {
    if (!atomic_exchange(flag, 1)) fprintf(stderr, "[hid] %s (handle %#x)\n", text, handle);
}
/* SDK handle bytes: style, player ID, device index. The desktop's first
   virtual full-key pad remains available to keyboard input; 2P requires its
   independent physical slot. Handheld preserves the existing 0x20 -> 1P map. */
static int handle_player(unsigned style, unsigned id) {
    if (style == STYLE_HANDHELD && id == 0x20 && runtime_handheld_mode()) return 0;
    if (style == STYLE_FULL_KEY && id < RUNTIME_GAMEPAD_PLAYERS && !runtime_handheld_mode()) {
        if (!id || (runtime_gamepad_connected_mask() & (1u << id))) return (int)id;
    }
    return -1;
}
static int vibration_channel(const uint32_t *handle) {
    if (!handle) runtime_fail("vibration device handle pointer");
    unsigned style = *handle & 0xff, id = (*handle >> 8) & 0xff, side = (*handle >> 16) & 0xff;
    int player = handle_player(style, id);
    if (!(*handle >> 24) && player >= 0 && side < 2) return player * 2 + (int)side;
    static _Atomic int warned; note_once(&warned, "vibration call with a handle this controller does not have; ignored", *handle);
    return -1;
}
static void play(int channel, const VibrationValue *value) {
    if (!value) runtime_fail("vibration value pointer");
    if (channel < 0) return;
    unsigned player = (unsigned)channel / 2, side = (unsigned)channel & 1;
    VibrationValue clean = *value;
    if (!isfinite(clean.amp_low) || !isfinite(clean.amp_high) || !isfinite(clean.freq_low) || !isfinite(clean.freq_high))
        clean = (VibrationValue){0, 160, 0, 320};
    if (runtime_input_suspended()) clean.amp_low = clean.amp_high = 0;
    pthread_mutex_lock(&lastLock); last[player][side] = clean; pthread_mutex_unlock(&lastLock);
    atomic_fetch_add(&values_in, 1);
    static _Atomic unsigned first; unsigned bit = 1u << player;
    if (!(atomic_fetch_or(&first, bit) & bit))
        fprintf(stderr, "[hid] player %u first vibration: %s %.2f@%.0f Hz %.2f@%.0f Hz\n", player + 1,
                side ? "right" : "left", clean.amp_low, clean.freq_low, clean.amp_high, clean.freq_high);
    if (atomic_load(&rumble_muted)) clean.amp_low = clean.amp_high = 0;
    runtime_rumble_set_player(player, side, clean.amp_low, clean.freq_low, clean.amp_high, clean.freq_high);
}
static uint32_t vib_init(const uint32_t *handle) {
    int channel = vibration_channel(handle); static _Atomic int once;
    if (channel >= 0) note_once(&once, "the game initialized its vibration devices", *handle);
    return 0;
}
static uint32_t vib_send(const uint32_t *handle, const VibrationValue *value) { play(vibration_channel(handle), value); return 0; }
static uint32_t vib_send_many(const uint32_t *handles, const VibrationValue *values, int count) {
    if (count < 0 || (count && (!handles || !values))) runtime_fail("vibration batch");
    for (int i = 0; i < count; i++) play(vibration_channel(&handles[i]), &values[i]);
    return 0;
}
static uint32_t vib_actual(VibrationValue *out, const uint32_t *handle) {
    if (!out) runtime_fail("vibration value output pointer");
    int channel = vibration_channel(handle); *out = (VibrationValue){0, 160, 0, 320};
    if (channel >= 0) { pthread_mutex_lock(&lastLock); *out = last[channel / 2][channel & 1]; pthread_mutex_unlock(&lastLock); }
    return 0;
}
static uint32_t vib_info(VibrationDeviceInfo *out, const uint32_t *handle) {
    if (!out) runtime_fail("vibration device info pointer");
    int channel = vibration_channel(handle);
    *out = (VibrationDeviceInfo){channel >= 0 ? 1u : 0u, channel >= 0 ? ((unsigned)channel & 1) + 1 : 0}; return 0;
}
static uint32_t vib_mounted(bool *out, const uint32_t *handle) {
    if (!out) runtime_fail("vibration mounted pointer"); *out = vibration_channel(handle) >= 0; return 0;
}
static uint32_t vib_permitted(bool *out) { if (!out) runtime_fail("vibration permitted pointer"); *out = true; return 0; }

static int six_axis_check(const uint32_t *handle) {
    if (!handle) runtime_fail("six-axis handle pointer");
    unsigned style = *handle & 0xff, id = (*handle >> 8) & 0xff, index = (*handle >> 16) & 0xff;
    int valid = !(*handle >> 24) && index == 2 && handle_player(style, id) == 0;
    if (!valid) { static _Atomic int warned; note_once(&warned, "six-axis handle is not the player 1 sensor; neutral state", *handle); }
    return valid;
}
static uint32_t six_start(const uint32_t *handle) {
    if (!six_axis_check(handle)) return 0;
    static _Atomic int once; note_once(&once, "the game started its player 1 six-axis sensor", *handle);
    runtime_motion_start(); return 0;
}
static uint32_t six_stop(const uint32_t *handle) { (void)six_axis_check(handle); return 0; }
static void neutral_six_axis(SixAxisState *out) {
    *out = (SixAxisState){.acc = {0, 0, -1}, .dir = {1, 0, 0, 0, 1, 0, 0, 0, 1}};
}
/* 2P has no advertised motion sensor in this implementation. A direct call
   with a 2P handle remains neutral and cannot read or reset the 1P fusion ring. */
static uint32_t six_states(int *out, SixAxisState *states, int max, const uint32_t *handle) {
    if (!out || (max > 0 && !states)) runtime_fail("six-axis state output");
    int valid = six_axis_check(handle); atomic_fetch_add(&reads, 1);
    if (max <= 0) { *out = 0; return 0; }
    RuntimeMotionState motion[RUNTIME_MOTION_MAX];
    int count = !valid || atomic_load(&gyro_muted) || runtime_input_suspended() ? 0 : runtime_motion_read(motion, max < RUNTIME_MOTION_MAX ? max : RUNTIME_MOTION_MAX);
    if (count <= 0) { neutral_six_axis(states); *out = 1; return 0; }
    for (int i = 0; i < count; i++) {
        SixAxisState *dest = &states[i];
        *dest = (SixAxisState){.delta_ns = (int64_t)motion[i].delta_ns, .sampling = (int64_t)motion[i].sampling, .attributes = SIX_AXIS_CONNECTED};
        memcpy(dest->acc, motion[i].acc, sizeof dest->acc); memcpy(dest->gyro, motion[i].gyro, sizeof dest->gyro);
        memcpy(dest->angle, motion[i].angle, sizeof dest->angle); memcpy(dest->dir, motion[i].dir, sizeof dest->dir);
    }
    *out = count; return 0;
}
static uint32_t six_state(SixAxisState *state, const uint32_t *handle) { int count; return six_states(&count, state, 1, handle); }
static int six_handles(uint32_t *out, int max, const uint32_t *id, uint32_t style_set) {
    if (max <= 0) return 0;
    if (!out || !id) runtime_fail("six-axis handle output");
    unsigned style = runtime_handheld_mode() ? STYLE_HANDHELD : STYLE_FULL_KEY;
    unsigned pad_id = runtime_handheld_mode() ? 0x20u : 0u;
    unsigned flag = runtime_handheld_mode() ? 2u : 1u;
    if (*id != pad_id || !(style_set & flag)) return 0;
    out[0] = style | (*id << 8) | (2u << 16); return 1;
}
static int no_handles(void *out, int max, const uint32_t *id, uint32_t style) { (void)out; (void)max; (void)id; (void)style; return 0; }

/* "flag rumble 0|1", "flag gyro 0|1": 1 if the name was one of these. */
int runtime_hid_feedback_flag(const char *name, int v) {
    _Atomic int *muted = !strcmp(name, "rumble") ? &rumble_muted : !strcmp(name, "gyro") ? &gyro_muted : NULL;
    if (!muted) return 0;
    if (v != 0 && v != 1) runtime_fail("flag rumble / gyro takes 0 or 1");
    int on = muted == &rumble_muted ? runtime_rumble_enabled() : runtime_gyro_enabled();
    atomic_store(muted, !v);
    if (!v && muted == &rumble_muted) runtime_feedback_quiet();
    fprintf(stderr, "[hid] %s %s%s\n", name, v ? "on" : "muted", on ? "" : " (no effect: the game has no such device in this run; start it with TOTK_RUMBLE=1 / TOTK_GYRO=1)");
    return 1;
}
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads) { *values = atomic_load(&values_in); *motion_reads = atomic_load(&reads); }

void *runtime_hid_feedback_override(const char *name) {
    const bool rumble = runtime_rumble_enabled(), gyro = runtime_gyro_enabled();
    if (!strcmp(name, "_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"))
        return rumble ? NULL : (void *)no_handles;   /* on: the SDK's own (nn::hid::utility) */
    if (!strcmp(name, "_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"))
        return gyro ? (void *)six_handles : (void *)no_handles;
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

/* Exercise the production HID dispatch and native routing with two isolated
   fake input slots. No real controller is registered by this probe process. */
extern int runtime_feedback_native_probe(void);
int runtime_hid_feedback_probe(void) {
    const char *owner = getenv("TOTK_SLOT_OWNER"), *probe = getenv("LM3_INPUT_PROBE");
    if (!owner || strcmp(owner, "codex") || !probe || strcmp(probe, "1")) runtime_fail("HID feedback probe requires isolated input probe");
    int checked = 0;
    const char *old = getenv("TOTK_RUMBLE"); char *saved_rumble = old ? strdup(old) : NULL;
    old = getenv("TOTK_GYRO"); char *saved_gyro = old ? strdup(old) : NULL;
    old = getenv("TOTK_GYRO_DEADZONE"); char *saved_deadzone = old ? strdup(old) : NULL;
    if (unsetenv("TOTK_GYRO_DEADZONE")) runtime_fail("HID feedback probe motion environment");
    if (setenv("TOTK_RUMBLE", "1", 1) || setenv("TOTK_GYRO", "1", 1)) runtime_fail("HID feedback probe environment");
    int saved_rumble_muted = atomic_exchange(&rumble_muted, 0), saved_gyro_muted = atomic_exchange(&gyro_muted, 0);
    RuntimeGamepadState connected = {.connected = 1};
    runtime_gamepad_probe_publish(0, &connected); runtime_gamepad_probe_publish(1, &connected);
    checked += runtime_feedback_native_probe();
    typedef uint32_t (*SendBatch)(const uint32_t *, const VibrationValue *, int);
    typedef uint32_t (*SendOne)(const uint32_t *, const VibrationValue *);
    typedef uint32_t (*ReadValue)(VibrationValue *, const uint32_t *);
    typedef uint32_t (*ReadInfo)(VibrationDeviceInfo *, const uint32_t *);
    typedef uint32_t (*Mounted)(bool *, const uint32_t *);
    typedef int (*SixHandles)(uint32_t *, int, const uint32_t *, uint32_t);
    typedef uint32_t (*SixRead)(SixAxisState *, const uint32_t *);
    typedef uint32_t (*SixStart)(const uint32_t *);
    SendBatch batch = (SendBatch)runtime_hid_feedback_override("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi");
    SendOne send = (SendOne)runtime_hid_feedback_override("_ZN2nn3hid6detail18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE");
    ReadValue actual = (ReadValue)runtime_hid_feedback_override("_ZN2nn3hid6detail23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE");
    ReadInfo info = (ReadInfo)runtime_hid_feedback_override("_ZN2nn3hid6detail22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE");
    Mounted mounted = (Mounted)runtime_hid_feedback_override("_ZN2nn3hid6detail24IsVibrationDeviceMountedEPbRKNS0_21VibrationDeviceHandleE");
    SixHandles handles = (SixHandles)runtime_hid_feedback_override("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE");
    SixRead sensor = (SixRead)runtime_hid_feedback_override("_ZN2nn3hid6detail21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE");
    SixStart start = (SixStart)runtime_hid_feedback_override("_ZN2nn3hid6detail18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE");
    if (!batch || !send || !actual || !info || !mounted || !handles || !sensor || !start) runtime_fail("HID feedback probe dispatch missing"); checked += 8;
    if (runtime_handheld_mode()) runtime_fail("two-player HID feedback probe requires desktop mode");
    const uint32_t ids[4] = {0x00010103u, 0x00000003u, 0x00000103u, 0x00010003u};
    const unsigned players_for[4] = {1, 0, 1, 0}, sides_for[4] = {1, 0, 0, 1};
    VibrationValue values[4] = {{0.1f, 80, 0.2f, 240}, {0.3f, 100, 0.4f, 260}, {0.5f, 120, 0.6f, 280}, {0.7f, 140, 0.8f, 300}};
    uint32_t routed[4]; memcpy(routed, ids, sizeof routed);
    const char *negative = getenv("TOTK_PROBE_NEGATIVE");
    if (negative && atoi(negative) == 123) for (unsigned i = 0; i < 4; i++) routed[i] &= ~0xff00u;
    batch(routed, values, 4);
    const uint64_t canary = UINT64_C(0xa5b6c7d8e9f01234);
    for (unsigned i = 0; i < 4; i++) {
        struct { uint64_t before; VibrationValue value; uint64_t after; } result = {.before = canary, .after = canary};
        actual(&result.value, &ids[i]);
        if (result.before != canary || result.after != canary || memcmp(&result.value, &values[i], sizeof result.value))
            runtime_fail("HID feedback probe cross-player GetActual or output overrun");
        checked += 3;
        float desired[4]; runtime_feedback_rumble_snapshot(players_for[i], sides_for[i], desired);
        if (memcmp(desired, &values[i], sizeof desired)) runtime_fail("HID feedback probe cross-player native wanted route"); checked++;
        VibrationDeviceInfo details; info(&details, &ids[i]); bool live = false; mounted(&live, &ids[i]);
        if (!live || details.type != 1 || details.position != sides_for[i] + 1) runtime_fail("HID feedback probe device metadata"); checked += 3;
    }
    /* Clearing one player must not mute the other player's two channels. */
    runtime_hid_feedback_clear_player(0);
    for (unsigned i = 0; i < 4; i++) {
        VibrationValue got; actual(&got, &ids[i]);
        VibrationValue expected = players_for[i] ? values[i] : (VibrationValue){0, 160, 0, 320};
        if (memcmp(&got, &expected, sizeof got)) runtime_fail("HID feedback probe per-player clear GetActual"); checked++;
        float desired[4]; runtime_feedback_rumble_snapshot(players_for[i], sides_for[i], desired);
        if (memcmp(desired, &expected, sizeof desired)) runtime_fail("HID feedback probe per-player clear wanted"); checked++;
    }
    const uint32_t invalid[] = {0x00000203u, 0x0000ff03u, 0x00020003u, 0x01000003u, 0x00000105u};
    const VibrationValue neutral = {0, 160, 0, 320};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        send(&invalid[i], &values[0]); VibrationValue got; actual(&got, &invalid[i]);
        bool live = true; mounted(&live, &invalid[i]); VibrationDeviceInfo details; info(&details, &invalid[i]);
        if (memcmp(&got, &neutral, sizeof got) || live || details.type || details.position) runtime_fail("HID feedback probe invalid handle accepted"); checked += 4;
    }
    RuntimeGamepadState disconnected = {0}; runtime_gamepad_probe_publish(1, &disconnected);
    VibrationValue gone; actual(&gone, &ids[0]); bool live = true; mounted(&live, &ids[0]);
    if (memcmp(&gone, &neutral, sizeof gone) || live) runtime_fail("HID feedback probe disconnected player 2 still mounted"); checked += 2;
    runtime_gamepad_probe_publish(1, &connected);
    uint32_t p1 = 0, p2 = 1, output = 0xdeadbeef;
    if (handles(&output, 1, &p1, 1) != 1 || output != 0x20003) runtime_fail("HID feedback probe player 1 sensor handle"); checked += 2;
    output = 0xdeadbeef;
    if (handles(&output, 1, &p2, 1) || output != 0xdeadbeef) runtime_fail("HID feedback probe player 2 advertised gyro"); checked += 2;
    if (handles(&output, 0, &p1, 1) || handles(&output, 1, &p1, 2)) runtime_fail("HID feedback probe sensor capacity/style"); checked += 2;
    uint32_t p1_sensor = 0x20003, p2_sensor = 0x20103;
    struct { uint64_t before; SixAxisState state; uint64_t after; } sample = {.before = canary, .after = canary};
    const double acc[3] = {0, 0, -1}, gyro[3] = {0, 0, 1}; uint64_t now = runtime_now_ns();
    if (runtime_input_suspended()) runtime_fail("HID feedback probe began with input suspended");
    if (runtime_motion_axes("x,y,z") < 0) runtime_fail("HID feedback probe motion axes");
    runtime_motion_deadzone_default(0);
    runtime_motion_live(1); runtime_motion_start();
    runtime_motion_feed(acc, gyro, now - 30000000ull); runtime_motion_feed(acc, gyro, now - 20000000ull);
    SixAxisState first; sensor(&first, &p1_sensor);
    if (!(first.attributes & SIX_AXIS_CONNECTED) || first.angle[2] <= 0) runtime_fail("HID feedback probe player 1 live motion"); checked += 2;
    start(&p2_sensor); /* Must not reset player 1's accumulated orientation. */
    runtime_motion_feed(acc, gyro, now - 10000000ull);
    sensor(&sample.state, &p2_sensor);
    SixAxisState neutral_motion; neutral_six_axis(&neutral_motion);
    if (sample.before != canary || sample.after != canary || memcmp(&sample.state, &neutral_motion, sizeof neutral_motion))
        runtime_fail("HID feedback probe player 2 read player 1 sensor or overran output");
    checked += 3;
    SixAxisState after; sensor(&after, &p1_sensor);
    if (!(after.attributes & SIX_AXIS_CONNECTED) || after.angle[2] <= first.angle[2] * 1.5f)
        runtime_fail("HID feedback probe player 2 sensor start reset player 1");
    checked += 2;
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_SETTINGS, 1);
    sensor(&sample.state, &p1_sensor);
    if (memcmp(&sample.state, &neutral_motion, sizeof neutral_motion)) runtime_fail("HID feedback probe suspended sensor not neutral"); checked++;
    send(&ids[1], &values[1]); actual(&gone, &ids[1]);
    if (gone.amp_low || gone.amp_high) runtime_fail("HID feedback probe suspended vibration not neutral"); checked++;
    runtime_input_suspend(RUNTIME_INPUT_SUSPEND_SETTINGS, 0);
    runtime_feedback_quiet();
    for (unsigned player = 0; player < RUNTIME_GAMEPAD_PLAYERS; player++) for (unsigned side = 0; side < 2; side++) {
        float desired[4]; runtime_feedback_rumble_snapshot(player, side, desired);
        if (memcmp(desired, &neutral, sizeof desired)) runtime_fail("HID feedback probe quiet left a channel active"); checked++;
    }
    runtime_motion_live(0); runtime_motion_start();
    runtime_gamepad_probe_publish(0, &disconnected); runtime_gamepad_probe_publish(1, &disconnected);
    atomic_store(&rumble_muted, saved_rumble_muted); atomic_store(&gyro_muted, saved_gyro_muted);
    if (saved_rumble) { setenv("TOTK_RUMBLE", saved_rumble, 1); free(saved_rumble); } else unsetenv("TOTK_RUMBLE");
    if (saved_gyro) { setenv("TOTK_GYRO", saved_gyro, 1); free(saved_gyro); } else unsetenv("TOTK_GYRO");
    if (saved_deadzone) { setenv("TOTK_GYRO_DEADZONE", saved_deadzone, 1); free(saved_deadzone); } else unsetenv("TOTK_GYRO_DEADZONE");
    const char *axes = getenv("TOTK_GYRO_AXES");
    if (runtime_motion_axes(axes ? axes : "x,y,z") < 0) runtime_fail("HID feedback probe restore motion axes");
    fprintf(stderr, "HID feedback probe: %d player/side routing, snapshot replacement, quiet and player 1 motion checks pass\n", checked);
    return checked;
}
