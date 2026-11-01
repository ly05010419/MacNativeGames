/* Real controller routing and saved policy, with the HID state supplied locally; no game files or physical pad needed. */
#include "../../runtime/gamepad.m"

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
uint64_t runtime_now_ns(void) { return 1000000000; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
void runtime_input_mouse(int button, int down) { (void)button; (void)down; }
int runtime_gyro_enabled(void) { return 0; }
void runtime_motion_mouse(int on) { (void)on; }
void runtime_motion_feed_mouse(const double a[3], const double g[3], uint64_t n) { (void)a; (void)g; (void)n; }
uint64_t runtime_keyboard_buttons(void) { return 0; }

static unsigned checks;
#define CHECK(x) do { checks++; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
static void write_word(const char *path, const char *word) { FILE *f = fopen(path, "w"); fputs(word, f); fclose(f); }

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    char controls[1024], saved[1024], word[32] = {0};
    snprintf(controls, sizeof controls, "%s/controls.txt", argv[1]);
    snprintf(saved, sizeof saved, "%s/controller-background.txt", argv[1]);
    setenv("TOTK_CONTROLS", controls, 1);
    load_controller_map();
    GCController.shouldMonitorBackgroundEvents = YES;
    load_gamepad_background();
    CHECK(!runtime_gamepad_background_enabled() && !GCController.shouldMonitorBackgroundEvents);
    CHECK(access(saved, F_OK) != 0);   /* loading the default does not create a preference */

    hidPad = (HIDPadState){.present = 1, .buttons = 1, .lx = 12345, .ry = -23456};
    refresh_gamepads();
    RuntimeGamepadState out; runtime_gamepad_read(&out);
    CHECK(out.buttons && out.lx == 12345 && out.ry == -23456);
    runtime_gamepad_focus(0); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !out.ry);
    hidPad.buttons = 2; refresh_gamepads(); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx);   /* the HID fallback cannot bypass the default-off policy */

    runtime_gamepad_background_set(1);
    CHECK(runtime_gamepad_background_enabled() && GCController.shouldMonitorBackgroundEvents);
    CHECK(runtime_setting_word("controller-background.txt", word, sizeof word) && !strcmp(word, "on\n"));
    runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 12345 && out.ry == -23456);
    runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 12345);   /* held through background frames */
    runtime_gamepad_focus(1); runtime_gamepad_focus(0); runtime_gamepad_read(&out);
    CHECK(out.buttons && out.lx == 12345);   /* opting in preserves input across focus loss */
    hidPad.buttons = 0; hidPad.lx = hidPad.ry = 0; refresh_gamepads(); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !out.ry);   /* releases are delivered in the background */

    hidPad.buttons = 1; hidPad.lx = 20000; refresh_gamepads();
    runtime_gamepad_background_set(0); runtime_gamepad_read(&out);
    CHECK(!out.buttons && !out.lx && !GCController.shouldMonitorBackgroundEvents);
    CHECK(runtime_setting_word("controller-background.txt", word, sizeof word) && !strcmp(word, "off\n"));
    refresh_gamepads(); runtime_gamepad_read(&out); CHECK(!out.buttons && !out.lx);
    runtime_gamepad_focus(1); runtime_gamepad_read(&out); CHECK(out.buttons && out.lx == 20000);

    /* Restart-equivalent preference loading; only a valid opt-in enables background events. */
    write_word(saved, "on\n"); load_gamepad_background();
    CHECK(runtime_gamepad_background_enabled() && GCController.shouldMonitorBackgroundEvents);
    write_word(saved, "off\n"); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    write_word(saved, "on garbage\n"); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    unlink(saved); load_gamepad_background(); CHECK(!runtime_gamepad_background_enabled());
    unsetenv("TOTK_CONTROLS"); runtime_gamepad_background_set(1);
    CHECK(runtime_gamepad_background_enabled() && access(saved, F_OK) != 0);
    printf("{\"status\":\"pass\",\"probe\":\"gamepad-background\",\"checks\":%u}\n", checks);
    return 0;
}}
