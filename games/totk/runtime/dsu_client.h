/* Included once by gamepad.m; the main queue owns this optional DSU input.
   Default off. A connected UDP socket accepts only the configured server peer. */
#if TARGET_OS_OSX
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include "dsu_protocol.h"

static DSUConfig dsuConfig;
static DSUInput dsuInput;
static DSUClock dsuClock;
static uint64_t dsuAt, dsuMotionAt, dsuSensorAt, dsuRegisterAt;
static int dsuSocket = -1;
static _Atomic int dsuMotionLive;
static uint32_t dsuClientID;
static dispatch_source_t dsuReader, dsuTimer;
static RuntimeDSUSettings dsuSettings;
static RuntimeDSUStatus dsuStatus;
static int dsuLoaded, dsuConnecting;
static unsigned dsuGeneration;
static void refresh_gamepads(void);
static void dsu_start(void);

static int dsu_word(const char *name, char *word, size_t size) {
    if (!runtime_setting_word(name, word, size)) return 0;
    size_t n = strlen(word), begin = 0;
    while (n && isspace((unsigned char)word[n - 1])) n--;
    while (begin < n && isspace((unsigned char)word[begin])) begin++;
    memmove(word, word + begin, n - begin); word[n - begin] = 0; return 1;
}
static void dsu_load(void) {
    if (dsuLoaded) return;
    dsuLoaded = 1; dsuSettings = (RuntimeDSUSettings){0};
    snprintf(dsuSettings.endpoint, sizeof dsuSettings.endpoint, "127.0.0.1:26760");
    char endpoint[264], slot[32] = "0", enabled[32];
    if (dsu_word("dsu-server.txt", endpoint, sizeof endpoint)) {
        DSUConfig check;
        if (dsu_config(endpoint, NULL, &check) == 1) snprintf(dsuSettings.endpoint, sizeof dsuSettings.endpoint, "%s", endpoint);
    }
    dsuSettings.enabled = dsu_word("dsu.txt", enabled, sizeof enabled) && !strcmp(enabled, "on");
    dsu_word("dsu-slot.txt", slot, sizeof slot);
    const char *env = getenv("TOTK_DSU"), *envSlot = getenv("TOTK_DSU_SLOT");
    dsuSettings.locked = env || envSlot;
    if (env) {
        dsuSettings.enabled = *env && strcmp(env, "0");
        if (dsuSettings.enabled) snprintf(dsuSettings.endpoint, sizeof dsuSettings.endpoint, "%s", !strcmp(env, "1") ? "127.0.0.1:26760" : env);
    }
    if (dsu_config(dsuSettings.endpoint, envSlot ? envSlot : slot, &dsuConfig) != 1) {
        dsuSettings.enabled = 0;
        snprintf(dsuStatus.error, sizeof dsuStatus.error, "Invalid DSU configuration");
        dsu_config("1", NULL, &dsuConfig);
    }
    dsuSettings.slot = dsuConfig.slot;
}
void runtime_dsu_settings(RuntimeDSUSettings *out) { dsu_load(); *out = dsuSettings; }
void runtime_dsu_status(RuntimeDSUStatus *out) {
    *out = dsuStatus;
    out->connected = dsuInput.connected && dsuAt && runtime_now_ns() - dsuAt <= 500000000ull;
    out->sensor = out->connected && dsuSensorAt && runtime_now_ns() - dsuSensorAt <= 200000000ull;
    if (out->connected) {
        out->buttons = dsuInput.buttons; memcpy(out->axes, dsuInput.axes, sizeof out->axes);
        memcpy(out->acc, dsuInput.acc, sizeof out->acc);
        for (int i = 0; i < 3; i++) out->gyro[i] = dsuInput.gyro[i] * 180 / 3.141592653589793;
    } else { out->buttons = 0; memset(out->axes, 128, sizeof out->axes); memset(out->acc, 0, sizeof out->acc); memset(out->gyro, 0, sizeof out->gyro); }
}

int runtime_dsu_motion_active(void) { return dsuMotionLive; }
static void dsu_mute_motion(void) {
    int owned = dsuMotionLive;
    dsuMotionLive = 0; dsuMotionAt = 0;
    if (owned) runtime_feedback_motion_source_changed();
}
static void dsu_stop_motion(void) {
    dsu_mute_motion(); dsuClock = (DSUClock){0};
}
static void dsu_quiet(void) {
    dsuAt = dsuSensorAt = 0; dsuInput = (DSUInput){0};
    dsu_stop_motion(); refresh_gamepads();
}
static void dsu_accept(const DSUInput *input, uint64_t now) {
    int same = dsuAt && input->server == dsuInput.server &&
        (!input->connected || !memcmp(input->mac, dsuInput.mac, sizeof input->mac));
    if (same && !dsu_sequence_after(input->sequence, dsuInput.sequence)) return;
    if (!input->connected) { dsu_quiet(); return; }
    if (!same) dsu_stop_motion();
    /* Pads without an IMU may send buttons with an empty sensor payload. */
    int sensor = input->timestamp && (input->acc[0] || input->acc[1] || input->acc[2] ||
                                     input->gyro[0] || input->gyro[1] || input->gyro[2]);
    if (sensor && (!same || input->timestamp != dsuInput.timestamp)) dsuSensorAt = now;
    dsuInput = *input; dsuAt = now;
    if (runtime_gyro_enabled() && sensor) {
        uint64_t stamp;
        int sample = dsu_sample_time(&dsuClock, input->timestamp, now, &stamp);
        if (sample) {
            if (!dsuMotionLive) {
                dsuMotionLive = 1;
                runtime_feedback_motion_source_changed();
            } else if (sample == 2) runtime_motion_live(0);
            if (sample == 2 || !dsuMotionAt) {
                runtime_motion_deadzone_default(0); runtime_motion_live(1);
            }
            runtime_motion_feed(input->acc, input->gyro, stamp); dsuMotionAt = now;
        }
    } else if (!runtime_gyro_enabled()) dsu_stop_motion();
    refresh_gamepads();
}
static void dsu_receive(void) {
    /* Bound each drain so a busy server cannot monopolize AppKit's main queue. */
    for (int i = 0; i < 64; i++) {
        uint8_t packet[513]; ssize_t n = recv(dsuSocket, packet, sizeof packet, 0);
        if (n < 0) { if (errno == EINTR) continue; break; }
        DSUInput input;
        if ((size_t)n <= 512 && dsu_decode(packet, (size_t)n, dsuConfig.slot, &input)) dsu_accept(&input, runtime_now_ns());
    }
}
static void dsu_tick(void) {
    uint64_t now = runtime_now_ns();
    if (!dsuRegisterAt || now - dsuRegisterAt >= 1000000000ull) {
        uint8_t request[28]; dsu_register(request, dsuClientID, dsuConfig.slot);
        (void)send(dsuSocket, request, sizeof request, 0); dsuRegisterAt = now;
    }
    if (dsuMotionLive && (!runtime_gyro_enabled() || now - dsuMotionAt > 200000000ull)) dsu_mute_motion();
    if (dsuAt && now - dsuAt > 500000000ull) dsu_quiet();
}
static void dsu_close(void) {
    dsuGeneration++; dsuConnecting = 0; dsu_stop_motion();
    if (dsuTimer) { dispatch_source_cancel(dsuTimer); dsuTimer = nil; }
    if (dsuReader) { dispatch_source_cancel(dsuReader); dsuReader = nil; }
    else if (dsuSocket >= 0) close(dsuSocket);
    dsuSocket = -1; dsuRegisterAt = 0;
}
int runtime_dsu_apply(const RuntimeDSUSettings *settings) {
    dsu_load(); if (dsuSettings.locked) return -2;
    if (settings->slot < 0 || settings->slot > 3 ||
        !memchr(settings->endpoint, 0, sizeof settings->endpoint)) return -1;
    char slot[2] = {(char)('0' + settings->slot), 0}; DSUConfig config;
    if (dsu_config(settings->endpoint, slot, &config) != 1) return -1;
    dsu_close(); dsu_quiet(); dsuStatus = (RuntimeDSUStatus){0};
    dsuSettings = *settings; dsuSettings.enabled = !!settings->enabled; dsuSettings.locked = 0; dsuConfig = config;
    runtime_setting_save("dsu-server.txt", dsuSettings.endpoint, "DSU server");
    runtime_setting_save("dsu-slot.txt", slot, "DSU slot");
    runtime_setting_save("dsu.txt", dsuSettings.enabled ? "on" : "off", "DSU input");
    dsu_start(); return 0;
}
static void dsu_resolved(struct addrinfo *addresses, int error, unsigned generation) {
    if (generation != dsuGeneration) { if (addresses) freeaddrinfo(addresses); return; }
    dsuConnecting = 0;
    if (error) { snprintf(dsuStatus.error, sizeof dsuStatus.error, "Cannot resolve server: %s", gai_strerror(error)); fprintf(stderr, "[dsu] %s; disabled\n", dsuStatus.error); return; }
    /* Most DSU servers bind IPv4 only. UDP connect cannot detect an absent
       IPv6 server, so prefer IPv4 for a hostname such as localhost. */
    for (int pass = 0; pass < 2 && dsuSocket < 0; pass++) {
        int family = pass ? AF_INET6 : AF_INET;
        for (struct addrinfo *a = addresses; a; a = a->ai_next) {
            if (a->ai_family != family) continue;
            int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) continue;
            if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0) { dsuSocket = fd; break; }
            close(fd);
        }
    }
    freeaddrinfo(addresses);
    if (dsuSocket < 0) { snprintf(dsuStatus.error, sizeof dsuStatus.error, "Cannot open server socket"); fprintf(stderr, "[dsu] %s; disabled\n", dsuStatus.error); return; }
    if (runtime_motion_axes(getenv("TOTK_GYRO_AXES")) < 0) { snprintf(dsuStatus.error, sizeof dsuStatus.error, "Invalid TOTK_GYRO_AXES"); fprintf(stderr, "[dsu] %s; disabled\n", dsuStatus.error); dsu_close(); return; }
    dsuClientID = arc4random(); static int exitHook; if (!exitHook) { atexit(dsu_close); exitHook = 1; }
    int fd = dsuSocket;
    dsuReader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)dsuSocket, 0, dispatch_get_main_queue());
    dispatch_source_set_cancel_handler(dsuReader, ^{ close(fd); });
    dispatch_source_set_event_handler(dsuReader, ^{ if (generation == dsuGeneration) dsu_receive(); }); dispatch_resume(dsuReader);
    dsuTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(dsuTimer, dispatch_time(DISPATCH_TIME_NOW, 0), 100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(dsuTimer, ^{ if (generation == dsuGeneration) dsu_tick(); }); dispatch_resume(dsuTimer);
    fprintf(stderr, "[dsu] server %s:%s, slot %u, subscribed to buttons, sticks and motion; waiting for valid six-axis samples\n",
            dsuConfig.host, dsuConfig.service, dsuConfig.slot);
}
static void dsu_start(void) {
    if (dsuSocket >= 0 || dsuConnecting) return;
    dsu_load(); if (!dsuSettings.enabled) return;
    dsuConnecting = 1;
    unsigned generation = dsuGeneration; DSUConfig config = dsuConfig;
    struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *addresses = NULL;
    /* Numeric addresses never require DNS; hostname lookup stays off AppKit's queue. */
    hints.ai_flags = AI_NUMERICHOST;
    int error = getaddrinfo(config.host, config.service, &hints, &addresses);
    if (!error) { dsu_resolved(addresses, 0, generation); return; }
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        struct addrinfo lookup = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_DGRAM}, *found = NULL;
        int result = getaddrinfo(config.host, config.service, &lookup, &found);
        dispatch_async(dispatch_get_main_queue(), ^{ dsu_resolved(found, result, generation); });
    });
}

#else
int runtime_dsu_motion_active(void) { return 0; }
void runtime_dsu_settings(RuntimeDSUSettings *out) { *out = (RuntimeDSUSettings){0}; }
void runtime_dsu_status(RuntimeDSUStatus *out) { *out = (RuntimeDSUStatus){0}; }
int runtime_dsu_apply(const RuntimeDSUSettings *settings) { (void)settings; return -1; }
static void dsu_start(void) {}
#endif
