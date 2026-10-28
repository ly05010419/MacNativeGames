/* runtime/gamepad.m's mouse mode (key 7): buttons through the controller layout, movement as the right stick and the gyro,
   release, and the saved on / off. The real file is compiled in; the 10 ms ticks are driven here (mouseTicksByHand). */
#include "../../runtime/gamepad.m"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
static uint64_t clockNow = 1000000000ull;
uint64_t runtime_now_ns(void) { return clockNow; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
static uint64_t keyboardButtons;   /* runtime/input.c: what the keys hold (the ability wheel: L) */
uint64_t runtime_keyboard_buttons(void) { return keyboardButtons; }
static int gyroOn = 1, motionMouse = -1, feeds, hookCalls;
static double lastYaw, lastAcc;
int runtime_gyro_enabled(void) { return gyroOn; }
void runtime_motion_mouse(int on) { motionMouse = on; }
void runtime_motion_feed_mouse(const double acc[3], const double gyro[3], uint64_t now) { (void)now; feeds++; lastYaw = gyro[2]; lastAcc = acc[2]; }
static void hook(void) { hookCalls++; }

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static int file_says(const char *path, const char *text) {
    char got[64] = {0}; FILE *f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(got, 1, sizeof got - 1, f); fclose(f); got[n] = 0; return !strcmp(got, text);
}
static RuntimeGamepadState settle(void) { RuntimeGamepadState s; runtime_gamepad_read(&s); runtime_gamepad_read(&s); return s; }   /* past latched presses */
static void spin(void) { [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]]; }
enum { Y = 1 << 3, L = 1 << 6, ZL = 1 << 8, ZR = 1 << 9 };

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) { fprintf(stderr, "usage: probe <empty folder>\n"); return 2; }
    char controls[1024], saved[1100];
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/mouse.txt", argv[1]);
    setenv("TOTK_CONTROLS", controls, 1);
    mouseTicksByHand = 1;
    controller_map_default(controllerMap, controllerModes);
    runtime_mouse_set_hook(hook);

    check(!runtime_mouse_enabled(), "mouse mode starts off");
    runtime_mouse_button(0, 1); runtime_mouse_move(10, 0); mouse_tick();
    RuntimeGamepadState s = settle();
    check(!s.buttons && !s.rx && !s.ry, "mouse mode off: clicks and movement do nothing");
    runtime_mouse_button(0, 0);

    check(runtime_mouse_set(1) == 1 && runtime_mouse_enabled() && file_says(saved, "on\n") && motionMouse == 1, "settings: on, saved, the mouse takes the gyro");
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
    /* The ability wheel (L held, here from the keys): the mouse points and the stick stays there; L up, centred. */
    keyboardButtons = L; mouse_tick(); runtime_mouse_move(10, 0); for (int i = 0; i < 30; i++) mouse_tick();
    int fed = feeds; s = settle();
    check(abs(s.rx - 16384) <= 1 && !s.ry && lastYaw == 0, "L held: 10 points right stays at half right, no gyro turn");
    runtime_mouse_move(0, -20); mouse_tick(); s = settle(); check(s.ry > 20000 && s.rx > 0 && s.rx < s.ry, "then up: up and a little right");
    keyboardButtons = 0; mouse_tick(); s = settle(); check(!s.rx && !s.ry && feeds > fed, "L let go: centred at once");

    check(runtime_mouse_set(0) == 0 && file_says(saved, "off\n") && motionMouse == 0, "settings: off, saved, the gyro back to the controller");
    spin(); check(hookCalls == 2, "the window is told again (to free the pointer)");
    FILE *f = fopen(saved, "w"); fputs("on\n", f); fclose(f);
    load_mouse_mode(); check(runtime_mouse_enabled() && motionMouse == 1, "the next start reads mouse mode on");
    runtime_mouse_set(0); f = fopen(saved, "w"); fputs("maybe\n", f); fclose(f);
    load_mouse_mode(); check(!runtime_mouse_enabled(), "a word other than on / off: off");

    setenv("TOTK_MOUSE_GYRO", "0", 1); motionMouse = -1;
    int before = feeds; runtime_mouse_set(1); runtime_mouse_move(10, 0); mouse_tick(); s = settle();
    check(motionMouse == 0 && feeds == before && s.rx > 0, "TOTK_MOUSE_GYRO=0: the stick only, the controller keeps the gyro");
    runtime_mouse_set(0); unsetenv("TOTK_MOUSE_GYRO");
    gyroOn = 0; motionMouse = -1; before = feeds; runtime_mouse_set(1); mouse_tick();
    check(motionMouse == 0 && feeds == before, "TOTK_GYRO=0 (no six-axis device for the game): no mouse gyro either");
    runtime_mouse_set(0);

    /* The settings panel's sensitivity: 50 with no file, a pick saved and applied in the middle of mouse mode, read back at
       the next start, a bad word falls back to 50, and TOTK_MOUSE_SENSITIVITY decides over all of it. */
    char sens[1100]; snprintf(sens, sizeof sens, "%s/mouse-sensitivity.txt", argv[1]);
    check(runtime_mouse_sensitivity() == 50 && !runtime_mouse_sensitivity_locked(), "sensitivity: 50 with no file");
    gyroOn = 1; runtime_mouse_set(1); runtime_mouse_sensitivity_set(100);
    runtime_mouse_move(5, 0); mouse_tick(); s = settle();
    check(file_says(sens, "100\n") && runtime_mouse_sensitivity() == 100 && abs(s.rx - 16384) <= 1, "sensitivity 100: saved, and 5 points is half the stick at once");
    runtime_mouse_sensitivity_set(0); runtime_mouse_sensitivity_set(101);
    check(runtime_mouse_sensitivity() == 100 && file_says(sens, "100\n"), "sensitivity out of 1-100: ignored");
    runtime_mouse_set(0);
    f = fopen(sens, "w"); fputs("30\n", f); fclose(f); atomic_store(&mouseSensitivity, 0);
    check(runtime_mouse_sensitivity() == 30, "the next start reads mouse-sensitivity.txt");
    f = fopen(sens, "w"); fputs("fast\n", f); fclose(f); atomic_store(&mouseSensitivity, 0);
    check(runtime_mouse_sensitivity() == 50, "a word other than 1-100: 50");
    setenv("TOTK_MOUSE_SENSITIVITY", "20", 1); runtime_mouse_sensitivity_set(90);
    check(runtime_mouse_sensitivity_locked() && runtime_mouse_sensitivity() == 20 && file_says(sens, "fast\n"), "TOTK_MOUSE_SENSITIVITY decides; a pick is not saved");
    unsetenv("TOTK_MOUSE_SENSITIVITY");

    printf("{\"status\":\"%s\",\"probe\":\"mouse\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}}
