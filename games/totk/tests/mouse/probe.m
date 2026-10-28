/* runtime/gamepad.m's mouse mode (key 7): buttons through the controller layout, movement as the right stick and the gyro,
   release, and the saved on / off. The real file is compiled in; the 10 ms ticks are driven here (mouseTicksByHand). */
#include "../../runtime/gamepad.m"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
static uint64_t clockNow = 1000000000ull;
uint64_t runtime_now_ns(void) { return clockNow; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
static int gyroOn = 1, motionMouse = -1, feeds, hookCalls;
static double lastYaw, lastAcc;
int runtime_gyro_enabled(void) { return gyroOn; }
void runtime_motion_mouse(int on) { motionMouse = on; }
void runtime_motion_feed_mouse(const double acc[3], const double gyro[3], uint64_t now) { (void)now; feeds++; lastYaw = gyro[2]; lastAcc = acc[2]; }
static void hook(void) { hookCalls++; }

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
/* settings.json, where every setting is (runtime/gamepad.m): the word under a key, as written. */
static int word_is(const char *path, const char *key, const char *word) {
    char got[2048] = {0}, want[80];
    FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(got, 1, sizeof got - 1, f); fclose(f); got[n] = 0;
    snprintf(want, sizeof want, "\"%s\" : \"%s\"", key, word);
    return strstr(got, want) != NULL;
}
static RuntimeGamepadState settle(void) { RuntimeGamepadState s; runtime_gamepad_read(&s); runtime_gamepad_read(&s); return s; }   /* past latched presses */
static void spin(void) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
enum { Y = 1 << 3, ZL = 1 << 8, ZR = 1 << 9 };

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: probe <empty folder>\n"); return 2; }
    char controls[1024], saved[1100];
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/settings.json", argv[1]);
    setenv("TOTK_CONTROLS", controls, 1);
    mouseTicksByHand = 1;
    controller_map_default(controllerMap, controllerModes);
    runtime_mouse_set_hook(hook);

    check(!runtime_mouse_enabled(), "mouse mode starts off");
    runtime_mouse_button(0, 1); runtime_mouse_move(10, 0); mouse_tick();
    RuntimeGamepadState s = settle();
    check(!s.buttons && !s.rx && !s.ry, "mouse mode off: clicks and movement do nothing");
    runtime_mouse_button(0, 0);

    check(runtime_mouse_set(1) == 1 && runtime_mouse_enabled() && word_is(saved, "mouse", "on") && motionMouse == 1, "settings: on, saved, the mouse takes the gyro");
    check(runtime_mouse_set(1) == 1 && motionMouse == 1, "on again: no change");
    spin(); check(hookCalls == 1, "the window is told (to capture the pointer)");
    runtime_mouse_button(0, 1); s = settle(); check(s.buttons == Y, "left button: attack (Y)");
    runtime_mouse_button(0, 0); s = settle(); check(!s.buttons, "left button up");
    runtime_mouse_button(1, 1); s = settle(); check(s.buttons == ZR, "right button held: draw the bow (ZR)");
    runtime_mouse_button(0, 1); s = settle(); check(!s.buttons, "left click while drawing: ZR let go (the shot), no attack");
    runtime_mouse_button(0, 0); s = settle(); check(s.buttons == ZR, "left up, right still held: drawing again");
    runtime_mouse_button(1, 0); runtime_mouse_button(2, 1); s = settle(); check(s.buttons == ZL, "middle button: target (ZL)");
    runtime_mouse_button(2, 0); runtime_mouse_button(9, 1); s = settle(); check(!s.buttons, "a button number out of range is ignored");

    runtime_mouse_move(10, 0); mouse_tick(); s = settle();
    check(abs(s.rx - 16384) <= 1 && !s.ry, "10 points right: half the right stick right");
    check(feeds == 1 && lastYaw < 0 && lastAcc == -1, "and the gyro turns clockwise, lying flat");
    mouse_tick(); s = settle(); check(s.rx > 0 && s.rx < 16384 && feeds == 2, "it decays tick by tick, the gyro keeps sampling");
    runtime_mouse_button(0, 1); runtime_mouse_release(); mouse_tick(); s = settle();
    check(!s.buttons && !s.rx && !s.ry, "release (the window lost the pointer): buttons up, stick centred");

    check(runtime_mouse_set(0) == 0 && word_is(saved, "mouse", "off") && motionMouse == 0, "settings: off, saved, the gyro back to the controller");
    spin(); check(hookCalls == 2, "the window is told again (to free the pointer)");
    FILE *f = fopen(saved, "w"); fputs("{\"mouse\":\"on\"}", f); fclose(f);
    settingsWords = nil;   /* the store reads once a session; a hand-written file is a new session */
    load_mouse_mode(); check(runtime_mouse_enabled() && motionMouse == 1, "the next start reads mouse mode on");
    runtime_mouse_set(0); f = fopen(saved, "w"); fputs("{\"mouse\":\"maybe\"}", f); fclose(f);
    settingsWords = nil;
    load_mouse_mode(); check(!runtime_mouse_enabled(), "a word other than on / off: off");

    setenv("TOTK_MOUSE_GYRO", "0", 1); motionMouse = -1;
    int before = feeds; runtime_mouse_set(1); runtime_mouse_move(10, 0); mouse_tick(); s = settle();
    check(motionMouse == 0 && feeds == before && s.rx > 0, "TOTK_MOUSE_GYRO=0: the stick only, the controller keeps the gyro");
    runtime_mouse_set(0); unsetenv("TOTK_MOUSE_GYRO");
    gyroOn = 0; motionMouse = -1; before = feeds; runtime_mouse_set(1); mouse_tick();
    check(motionMouse == 0 && feeds == before, "TOTK_GYRO=0 (no six-axis device for the game): no mouse gyro either");
    runtime_mouse_set(0);

    printf("{\"status\":\"%s\",\"probe\":\"mouse\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}}
