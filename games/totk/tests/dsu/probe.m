/* Real UDP receiver and actual gamepad routing; no game files or physical pad. */
#include "../../runtime/gamepad.m"
#include <assert.h>
#include <objc/runtime.h>
#include <poll.h>

static uint64_t clockNow = 1000000000ull, fedStamp;
static int gyroOn = 1, feeds, motionLive;
static int sourceChanges;
void runtime_feedback_motion_source_changed(void) { sourceChanges++; runtime_motion_live(0); }
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL: %s\n", s); exit(1); }
uint64_t runtime_now_ns(void) { return clockNow; }
int runtime_gyro_enabled(void) { return gyroOn; }
void runtime_motion_feed(const double a[3], const double g[3], uint64_t t) { (void)a; (void)g; feeds++; fedStamp = t; }
void runtime_motion_live(int on) { motionLive = on; }
void runtime_motion_deadzone_default(double v) { assert(v == 0); }
int runtime_motion_axes(const char *axes) { (void)axes; return 0; }
void runtime_motion_mouse(int on) { (void)on; }
void runtime_motion_feed_mouse(const double a[3], const double g[3], uint64_t t) { (void)a; (void)g; (void)t; }
void runtime_feedback_attach(GCController *c) { (void)c; }
void runtime_feedback_detach(GCController *c) { (void)c; }
void runtime_feedback_quiet(void) {}
void runtime_input_mouse(int button, int down) { (void)button; (void)down; }
uint64_t runtime_keyboard_buttons(void) { return 0; }

static RuntimeGamepadState state(void) { RuntimeGamepadState s; runtime_gamepad_read(&s); runtime_gamepad_read(&s); return s; }
static void seal(uint8_t *p) { dsu_put32(p + 8, dsu_crc(p, 100)); }
static NSArray *no_controllers(id object, SEL selector) { (void)object; (void)selector; return @[]; }
int main(int argc, char **argv) { @autoreleasepool {
    assert(argc == 3);
    FILE *f = fopen(argv[1], "rb"); assert(f); uint8_t p[100]; assert(fread(p, 1, 100, f) == 100); fclose(f);
    char controls[1024]; snprintf(controls, sizeof controls, "%s/controls.txt", argv[2]);
    setenv("TOTK_CONTROLS", controls, 1); controller_map_default(controllerMap, controllerModes);
    method_setImplementation(class_getClassMethod(GCController.class, @selector(controllers)), (IMP)no_controllers);
    unsetenv("TOTK_DSU"); dsu_start(); assert(dsuSocket < 0);
    setenv("TOTK_DSU", "bad", 1); dsuLoaded = 0; dsu_start(); assert(dsuSocket < 0);
    int server = socket(AF_INET, SOCK_DGRAM, 0); assert(server >= 0);
    struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(server, (struct sockaddr *)&address, sizeof address)); socklen_t size = sizeof address;
    assert(!getsockname(server, (struct sockaddr *)&address, &size));
    char endpoint[64]; snprintf(endpoint, sizeof endpoint, "localhost:%u", ntohs(address.sin_port));
    setenv("TOTK_DSU", endpoint, 1); setenv("TOTK_DSU_SLOT", "2", 1); dsuLoaded = 0; dsu_start();
    for (int i = 0; i < 200 && dsuSocket < 0; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
    assert(dsuSocket >= 0 && !runtime_dsu_motion_active() && sourceChanges == 0);
    struct timeval timeout = {.tv_sec = 1}; setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    dsu_tick(); uint8_t request[28]; struct sockaddr_storage peer; socklen_t peerSize = sizeof peer;
    assert(recvfrom(server, request, sizeof request, 0, (struct sockaddr *)&peer, &peerSize) == 28);
    assert(!memcmp(request, "DSUC", 4) && request[21] == 2 && dsu_u32(request + 8) == dsu_crc(request, sizeof request));
    DSUInput decoded; assert(dsu_decode(p, sizeof p, 2, &decoded));
    /* A reachable server and live buttons do not prove that it has an IMU. */
    uint8_t buttonsOnly[100]; memcpy(buttonsOnly, p, sizeof buttonsOnly);
    dsu_put32(buttonsOnly + 32, 0); memset(buttonsOnly + 68, 0, 32); buttonsOnly[68] = 1; seal(buttonsOnly);
    assert(sendto(server, buttonsOnly, 100, 0, (struct sockaddr *)&peer, peerSize) == 100);
    struct pollfd first = {.fd = dsuSocket, .events = POLLIN}; assert(poll(&first, 1, 1000) == 1); dsu_receive();
    RuntimeDSUStatus waiting; runtime_dsu_status(&waiting);
    assert(waiting.connected && waiting.buttons && !waiting.sensor && !runtime_dsu_motion_active() && !sourceChanges);
    memcpy(buttonsOnly, p, sizeof buttonsOnly); memset(buttonsOnly + 68, 0, 8); seal(buttonsOnly);
    assert(sendto(server, buttonsOnly, 100, 0, (struct sockaddr *)&peer, peerSize) == 100);
    assert(poll(&first, 1, 1000) == 1); dsu_receive();
    assert(!runtime_dsu_motion_active() && !sourceChanges); // no sensor timestamp yet
    dsu_put32(p + 32, 2); seal(p);
    assert(sendto(server, p, sizeof p, 0, (struct sockaddr *)&peer, peerSize) == 100);
    struct pollfd ready = {.fd = dsuSocket, .events = POLLIN}; assert(poll(&ready, 1, 1000) == 1);
    dsu_receive();
    RuntimeGamepadState s = state();
    assert(dsuInput.connected && s.connected && s.buttons == (1u << 1) && s.lx == 32767);
    assert(feeds == 1 && motionLive && runtime_dsu_motion_active() && sourceChanges == 1 && fedStamp == clockNow); // Python fixture has another machine's epoch
    dsu_receive(); assert(feeds == 1);
    int wrongPeer = socket(AF_INET, SOCK_DGRAM, 0); assert(wrongPeer >= 0);
    p[37] = 0x40; seal(p); assert(sendto(wrongPeer, p, 100, 0, (struct sockaddr *)&peer, peerSize) == 100);
    dsu_receive(); assert(state().buttons == (1u << 1)); close(wrongPeer);
    DSUInput input = dsuInput; input.sequence++; input.timestamp += 10000; clockNow += 30000000;
    dsu_accept(&input, clockNow); assert(feeds == 2 && fedStamp == 1010000000ull);
    dsu_accept(&input, clockNow); assert(feeds == 2); // duplicate sequence
    input.sequence--; input.connected = 0; dsu_accept(&input, clockNow); assert(dsuInput.connected); // reordered disconnect
    input = dsuInput; input.sequence++; input.timestamp += 10000;
    runtime_gamepad_focus(0); dsu_accept(&input, clockNow); assert(!state().buttons && !state().lx);
    runtime_gamepad_focus(1); assert(state().buttons == (1u << 1));
    input.sequence++; input.timestamp += 10000; clockNow += 10000000; dsu_accept(&input, clockNow); assert(motionLive);
    gyroOn = 0; dsu_tick(); assert(!motionLive);
    input.sequence++; input.timestamp += 10000; clockNow += 10000000; dsu_accept(&input, clockNow);
    RuntimeDSUStatus raw; runtime_dsu_status(&raw); assert(raw.connected && raw.sensor && !motionLive && raw.gyro[0] > 9);
    gyroOn = 1;
    input.sequence++; input.timestamp += 10000; clockNow += 10000000; dsu_accept(&input, clockNow); assert(motionLive);
    clockNow += 210000000; dsu_tick(); assert(!motionLive && !runtime_dsu_motion_active() && dsuInput.connected); // hand motion back independently of buttons
    int before = feeds; input.sequence++; dsu_accept(&input, clockNow); assert(!motionLive && feeds == before); // duplicate timestamps cannot resurrect motion
    assert(!runtime_dsu_motion_active());
    input.sequence++; input.timestamp += 10000; dsu_accept(&input, clockNow);
    assert(runtime_dsu_motion_active() && motionLive && feeds == before + 1); // fresh sensors take over again
    clockNow += 510000000; dsu_tick(); s = state(); assert(!s.buttons && !s.lx && !dsuInput.connected);
    input.server++; input.sequence = 0; input.timestamp = 7; dsu_accept(&input, clockNow);
    assert(dsuInput.connected && motionLive && fedStamp == clockNow); // restart with a new epoch
    input.sequence++; input.connected = 0; dsu_accept(&input, clockNow); assert(!state().buttons && !motionLive);
    dsu_close(); dsu_quiet();
    unsetenv("TOTK_DSU"); unsetenv("TOTK_DSU_SLOT"); dsuLoaded = 0;
    sourceChanges = 0;
    setenv("TOTK_DSU_BUTTONS", "0", 1); // old motion-only overrides no longer suppress buttons or lock settings
    runtime_setting_save("dsu-buttons.txt", "0", "legacy fixture");
    RuntimeDSUSettings settings; runtime_dsu_settings(&settings); assert(!settings.enabled && !settings.locked);
    snprintf(settings.endpoint, sizeof settings.endpoint, "127.0.0.1:%u", ntohs(address.sin_port)); settings.enabled = 1; settings.slot = 2;
    assert(!runtime_dsu_apply(&settings) && dsuSocket >= 0 && sourceChanges == 0);
    dsu_tick(); peerSize = sizeof peer;
    assert(recvfrom(server, request, sizeof request, 0, (struct sockaddr *)&peer, &peerSize) == 28);
    p[37] = 0x20; seal(p); assert(sendto(server, p, 100, 0, (struct sockaddr *)&peer, peerSize) == 100);
    ready.fd = dsuSocket; assert(poll(&ready, 1, 1000) == 1); dsu_receive();
    RuntimeDSUStatus status; runtime_dsu_status(&status);
    assert(status.connected && status.sensor && status.buttons && motionLive);
    settings.enabled = 0; assert(!runtime_dsu_apply(&settings) && dsuSocket < 0 && sourceChanges == 2);
    runtime_dsu_status(&status); assert(!status.connected && !status.sensor && !motionLive && !state().buttons);
    dsuLoaded = 0; runtime_dsu_settings(&settings); assert(!settings.enabled && settings.slot == 2 && strstr(settings.endpoint, "127.0.0.1:") == settings.endpoint);
    settings.enabled = 1; snprintf(settings.endpoint, sizeof settings.endpoint, "bad");
    assert(runtime_dsu_apply(&settings) == -1 && dsuSocket < 0);
    snprintf(settings.endpoint, sizeof settings.endpoint, "%s", endpoint); settings.enabled = 1;
    assert(!runtime_dsu_apply(&settings)); // DNS is asynchronous
    settings.enabled = 0; assert(!runtime_dsu_apply(&settings));
    for (int i = 0; i < 20; i++) CFRunLoopRunInMode(kCFRunLoopDefaultMode, .01, false);
    assert(dsuSocket < 0); // a cancelled lookup cannot restore the old connection
    setenv("TOTK_DSU", "0", 1); dsuLoaded = 0; runtime_dsu_settings(&settings);
    assert(settings.locked && runtime_dsu_apply(&settings) == -2);
    close(server); dsu_close();
    puts("DSU UDP/routing/focus/timeout/restart tests passed");
    return 0;
} }
