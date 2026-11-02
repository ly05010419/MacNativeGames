/* Exercise the actual host mapping and guest HID with independent snapshot controllers. */
#include "../../runtime/gamepad.m"
#include "../../runtime/input.c"
#include <assert.h>
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
uint64_t runtime_now_ns(void) { return 0; }
void runtime_feedback_attach(GCController *controller) { (void)controller; }
void runtime_feedback_detach(GCController *controller) { (void)controller; }
void runtime_feedback_quiet(void) {}
int runtime_gyro_enabled(void) { return 0; }
void runtime_motion_mouse(int on) { (void)on; }
void runtime_motion_feed_mouse(const double acc[3], const double gyro[3], uint64_t now) { (void)acc; (void)gyro; (void)now; }


static int handheld;
int runtime_handheld_mode(void) { return handheld; }
void runtime_survey_cancel_user(void) {}
void runtime_autoload_user_input(void) {}
void runtime_shortcut_help_toggle(void) {}
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
void taa_toggle(void) {}

int runtime_survey_command(const char *line) { (void)line; return 0; }
void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }
void runtime_front_request_close(void) {}
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_gpucapture_next(const char *path) { (void)path; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long n) { (void)n; }
int runtime_hid_feedback_flag(const char *name, int v) { (void)name; (void)v; return 0; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void runtime_swkbd_selftest(void) {}
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_hle_trace_set(int mode) { (void)mode; }
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_dump_threads(void) {}

int main(void) { @autoreleasepool {
    controller_map_default(controllerMap, controllerModes);
    GCController *one = [GCController controllerWithExtendedGamepad];
    GCController *two = [GCController controllerWithExtendedGamepad];
    attach_gamepad(one); attach_gamepad(two);
    initialize(); uint32_t ids[] = {0, 1}; supported_ids(ids, 2);
    NpadState a, b;
    assert(style(&ids[0]) == 1 && style(&ids[1]) == 1);
    [one.extendedGamepad.leftThumbstick setValueForXAxis:1 yAxis:0];
    [two.extendedGamepad.leftThumbstick setValueForXAxis:-1 yAxis:0];
    [two.extendedGamepad.buttonA setValue:1]; refresh_gamepads();
    full_states(&a, 1, &ids[0]); full_states(&b, 1, &ids[1]);
    assert(a.lx == 32767 && b.lx == -32767);
    assert(!(a.buttons & 15) && (b.buttons & 15));
    /* Player 1 reads must not consume a short player 2 press. */
    [two.extendedGamepad.buttonA setValue:0]; refresh_gamepads();
    [two.extendedGamepad.buttonB setValue:1]; refresh_gamepads();
    [two.extendedGamepad.buttonB setValue:0]; refresh_gamepads();
    full_states(&a, 1, &ids[0]); full_states(&b, 1, &ids[1]);
    assert(!(a.buttons & 15) && (b.buttons & 15));
    full_states(&b, 1, &ids[1]); assert(!(b.buttons & 15));
    runtime_input_key_synthetic(38, 1);
    full_states(&b, 1, &ids[1]); full_states(&a, 1, &ids[0]);
    assert((a.buttons & 1) && !(b.buttons & 1));
    runtime_input_key_synthetic(38, 0);
    ControllerSupportResult result; int8_t limits[] = {2, 2};
    show_controller_support(&result, limits); assert(result.players == 2);
    limits[1] = 1; show_controller_support(&result, limits);
    assert(result.players == 1 && !style(&ids[1]));
    limits[1] = 2; show_controller_support(&result, limits);
    assert(style(&ids[1]) == 1);
    supported_ids(ids, 1); show_controller_support(&result, limits);
    assert(!style(&ids[1])); supported_ids(ids, 2);
    /* Disconnecting player 1 must leave player 2 in its original slot. */
    detach_gamepad(one);
    assert(playerControllers[1] == two);
    GCController *replacement = [GCController controllerWithExtendedGamepad];
    attach_gamepad(replacement); assert(playerControllers[0] == replacement);
    detach_gamepad(two);
    full_states(&b, 1, &ids[1]); assert(!b.attributes && !b.buttons && !b.lx);
    assert(!style(&ids[1]));
    attach_gamepad(two); assert(style(&ids[1]) == 1);
    runtime_gamepad_clear(); assert(runtime_gamepad_connected(1));
    handheld = 1; uint32_t handheldID = 0x20; supported_styles(3);
    assert(style(&handheldID) == 2 && !style(&ids[0]) && !style(&ids[1]));
    puts("{\"status\":\"pass\",\"probe\":\"two-player\"}");
    return 0;
}}
