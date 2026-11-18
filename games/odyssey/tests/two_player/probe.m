/* Two GameController snapshots through Odyssey's host mapping and the guest Npad API. */
#include "../../runtime/gamepad.m"
#include "../../runtime/input.c"
#include "../../runtime/hid_feedback.c"
#include <assert.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
static uint64_t now = 1000000000;
uint64_t runtime_now_ns(void) { return now; }
static int handheld, quiet, motionReads, motionStarts, rumbles;
int runtime_handheld_mode(void) { return handheld; }
static GCController *lastFeedbackCandidate;
void runtime_feedback_attach(GCController *c) { lastFeedbackCandidate = c; }
void runtime_feedback_detach(GCController *c) { if (lastFeedbackCandidate == c) lastFeedbackCandidate = nil; }
void runtime_feedback_quiet(void) { quiet++; }
void runtime_autoload_user_input(void) {}
void runtime_survey_cancel_user(void) {}
void runtime_settings_toggle(void) {}
int runtime_motion_read(RuntimeMotionState *out, int max) { (void)out; (void)max; motionReads++; return 0; }
void runtime_motion_start(void) { motionStarts++; }
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    (void)side; (void)al; (void)fl; (void)ah; (void)fh; rumbles++;
}

static NpadState read_pad(uint32_t id) {
    NpadState state;
    full_states(&state, 1, &id);
    return state;
}

/* Check the host before any reconnect: a disconnected guest Npad reads neutral even with stale host input. */
static void assert_host_clear(int player) {
    RuntimeGamepadState pad;
    runtime_gamepad_read_player(player, &pad);
    assert(pad.connected && !pad.buttons && !pad.lx && !pad.ly && !pad.rx && !pad.ry);
}

/* Controller support runs on the guest thread; feedback ownership changes on the main queue. */
static void drain_main_queue(void) {
    dispatch_async(dispatch_get_main_queue(), ^{ CFRunLoopStop(CFRunLoopGetMain()); });
    CFRunLoopRun();
}

static void guest_disconnect_test(GCExtendedGamepad *one, GCExtendedGamepad *two) {
    void (*disconnectNpad)(const uint32_t *) = runtime_input_override("_ZN2nn3hid14DisconnectNpadERKj");
    assert(disconnectNpad);
    uint32_t ids[] = {0, 1};
    ControllerSupportArgs arg = {.min = 2, .max = 2};
    RuntimeGamepadState touch = {.connected = 1, .buttons = 8, .ry = 16000};
    [two.leftThumbstick setValueForXAxis:-1 yAxis:1];
    [one.rightThumbstick setValueForXAxis:1 yAxis:0]; refresh_gamepads();
    runtime_input_key(38, 1); runtime_input_key_synthetic(36, 1);
    runtime_touch_pad_set(&touch);
    NpadState combined = read_pad(0);
    assert(combined.lx == -32767 && combined.ly == 32767 && combined.rx == 32767 && combined.ry == 16000);
    assert((combined.buttons & 1033) == 1033);  /* A, +, touch Y */
    [two.leftShoulder setValue:1]; refresh_gamepads();
    [two.leftShoulder setValue:0]; refresh_gamepads();  /* an unread short L */
    int before = quiet;
    disconnectNpad(&ids[0]);
    assert(!style(&ids[0]) && !read_pad(0).attributes && !runtime_keyboard_buttons() && quiet == before + 1);
    assert_host_clear(0);
    supported_ids(ids, 2);
    assert(read_pad(0).attributes && !read_pad(0).buttons);

    assert(show_controller_support(NULL, &arg) == 0);
    [one.leftThumbstick setValueForXAxis:1 yAxis:0]; refresh_gamepads();
    runtime_input_key(38, 1);
    [one.rightShoulder setValue:1]; [two.leftShoulder setValue:1]; refresh_gamepads();
    [one.rightShoulder setValue:0]; [two.leftShoulder setValue:0]; refresh_gamepads();
    before = quiet;
    disconnectNpad(&ids[1]);
    assert(!style(&ids[1]) && !read_pad(1).attributes && quiet == before);
    assert_host_clear(1);
    NpadState mario = read_pad(0);
    assert(mario.lx == 32767 && (mario.buttons & 129) == 129);  /* held key and unread R survived */
    [two.leftShoulder setValue:1]; refresh_gamepads();
    [two.leftShoulder setValue:0]; refresh_gamepads();  /* pressed while the guest had P2 disconnected */
    assert(!show_controller_support(NULL, &arg) && style(&ids[1]) && !read_pad(1).buttons);

    [two.leftShoulder setValue:1]; refresh_gamepads();
    [two.leftShoulder setValue:0]; refresh_gamepads();
    before = quiet;
    disconnectNpad(&ids[0]);
    assert(!style(&ids[0]) && !read_pad(0).attributes && !runtime_keyboard_buttons() && quiet == before + 1);
    assert_host_clear(0);
    NpadState cappy = read_pad(1);
    assert(cappy.lx == -32767 && (cappy.buttons & 64));  /* held stick and unread L survived */

    assert(!show_controller_support(NULL, &arg));
    GCController *extra = [GCController controllerWithExtendedGamepad];
    attach_gamepad(extra);
    [extra.extendedGamepad.rightThumbstick setValueForXAxis:0 yAxis:-1]; refresh_gamepads();
    disconnectNpad(&ids[1]);
    ControllerSupportArgs solo = {.max = 1, .singleMode = 1};
    assert(!show_controller_support(NULL, &solo));
    NpadState sharedMario = read_pad(0);
    assert(!sharedMario.ly && sharedMario.ry == -32767);  /* cleared Cappy stick, third pad still helps Mario */
    detach_gamepad(extra);
    assert(!show_controller_support(NULL, &arg));
    runtime_gamepad_settings_enable(1);
    RuntimeGamepadState ui; runtime_gamepad_text_read(&ui); /* discard earlier navigation events */
    [one.leftThumbstickButton setValue:1]; [one.rightThumbstickButton setValue:1];
    [one.leftShoulder setValue:1]; refresh_gamepads();
    [one.leftShoulder setValue:0]; refresh_gamepads();
    disconnectNpad(&ids[1]);
    assert(runtime_gamepad_settings_requested()); /* P1's pending panel request */
    runtime_gamepad_text_read(&ui);
    assert(ui.buttons & 64);                     /* P1's short press between UI polls */
    refresh_gamepads();
    assert(!runtime_gamepad_settings_requested()); /* P1 never released the shortcut */
    puts("pass: DisconnectNpad clears shared 1P, P2 alone, and P1 alone");
}

int main(int argc, char **argv) { @autoreleasepool {
    unsetenv("TOTK_CONTROLS");   /* exercise remapping without reading or writing the player's layout */
    controller_map_default(controllerMap, controllerModes);
    GCController *one = [GCController controllerWithExtendedGamepad];
    GCController *two = [GCController controllerWithExtendedGamepad];
    attach_gamepad(one);
    attach_gamepad(two);
    uint32_t ids[] = {0, 1}, handheldID = 0x20;
    initialize(); supported_ids(ids, 2);
    if (argc == 2 && !strcmp(argv[1], "--disconnect")) {
        guest_disconnect_test(one.extendedGamepad, two.extendedGamepad);
        return 0;
    }
    ControllerSupportResult result;
    ControllerSupportArgs arg = {.min = 2, .max = 2};
    assert(lastFeedbackCandidate == two);  /* later pads may still supply a missing capability */
    assert(style(&ids[0]) && !style(&ids[1]));
    [two.extendedGamepad.leftThumbstick setValueForXAxis:-1 yAxis:0];
    refresh_gamepads();
    assert(read_pad(0).lx == -32767 && !read_pad(1).attributes);
    [one.extendedGamepad.leftThumbstick setValueForXAxis:0 yAxis:1];
    refresh_gamepads();
    assert(read_pad(0).ly == 32767);
    arg.min = 1; show_controller_support(&result, &arg);
    assert(result.players == 1 && !style(&ids[1]));    /* an optional range on startup */
    arg.min = 2;
    assert(show_controller_support(&result, &arg) == 0);
    drain_main_queue(); assert(lastFeedbackCandidate == one);
    assert(result.players == 2 && result.selected == 0 && style(&ids[1]));
    assert(read_pad(0).lx == 0 && read_pad(0).ly == 32767);
    assert(read_pad(1).lx == -32767 && read_pad(1).ly == 0);

    /* The live Switch fallback and its expiry also apply to the panel, without moving Cappy. */
    runtime_gamepad_switch_right((__bridge void *)one, 4095, 2048);
    now += 210000000;
    runtime_gamepad_switch_right((__bridge void *)one, 4095, 2048);
    RuntimeGamepadState ui; runtime_gamepad_text_read(&ui);
    assert(read_pad(0).rx == 32767 && !read_pad(1).rx && ui.rx == 32767);
    now += 251000000;
    runtime_gamepad_text_read(&ui); assert(!read_pad(0).rx && !ui.rx);

    RuntimeGamepadState touch = {.connected = 1, .buttons = 8};
    runtime_touch_pad_set(&touch); touch.buttons = 0; runtime_touch_pad_set(&touch);
    assert(!(read_pad(1).buttons & 8) && (read_pad(0).buttons & 8));

    [two.extendedGamepad.leftShoulder setValue:1]; refresh_gamepads();
    [two.extendedGamepad.leftShoulder setValue:0]; refresh_gamepads();
    runtime_input_key(38, 1);
    assert((read_pad(0).buttons & 65) == 1);
    assert((read_pad(1).buttons & 65) == 64);
    assert(!(read_pad(1).buttons & 65));

    /* A per-button tap bound by the settings panel persists only on the seat that pressed it. */
    uint16_t map[CONTROLLER_BUTTONS]; uint8_t modes[CONTROLLER_BUTTONS];
    runtime_controller_map_get(map, modes);
    modes[6] = CONTROLLER_TAP; map[6] = 1u << 2;
    runtime_controller_map_set(map, modes);
    [two.extendedGamepad.leftShoulder setValue:1]; refresh_gamepads();
    [two.extendedGamepad.leftShoulder setValue:0]; refresh_gamepads();
    assert(!(read_pad(0).buttons & 4) && (read_pad(1).buttons & 4));
    assert(read_pad(1).buttons & 4);
    runtime_controller_map_reset();

    /* While the panel owns both controllers, it sees the raw buttons and neither Npad gets them. */
    [one.extendedGamepad.leftThumbstick setValueForXAxis:0 yAxis:0];
    [two.extendedGamepad.leftThumbstick setValueForXAxis:0 yAxis:0];
    runtime_gamepad_settings_enable(1);
    [one.extendedGamepad.leftThumbstickButton setValue:1];
    [two.extendedGamepad.rightThumbstickButton setValue:1]; refresh_gamepads();
    assert(!runtime_gamepad_settings_requested());
    [two.extendedGamepad.leftThumbstickButton setValue:1]; refresh_gamepads();
    assert(runtime_gamepad_settings_requested());
    runtime_gamepad_text_capture(1);
    runtime_gamepad_text_read(&ui);
    assert((ui.buttons & SETTINGS_CHORD) == SETTINGS_CHORD);
    assert(!(read_pad(0).buttons & SETTINGS_CHORD) && !read_pad(1).buttons);
    runtime_gamepad_text_capture(0);
    [one.extendedGamepad.leftThumbstickButton setValue:0];
    [two.extendedGamepad.leftThumbstickButton setValue:0];
    [two.extendedGamepad.rightThumbstickButton setValue:0]; refresh_gamepads();
    read_pad(0); assert(!read_pad(1).buttons);

    [two.extendedGamepad.leftThumbstick setValueForXAxis:-1 yAxis:0]; refresh_gamepads();
    /* No P2 handle may read, restart or vibrate Mario's single feedback backend. */
    uint32_t p1Handle = 3, p2Handle = 3 | 1u << 8;
    VibrationValue buzz = {.amp_low = 0.5f}; SixAxisState motion;
    vib_send(&p2Handle, &buzz); six_start(&p2Handle); six_state(&motion, &p2Handle);
    assert(!rumbles && !motionStarts && !motionReads && !motion.attributes);
    vib_send(&p1Handle, &buzz); six_start(&p1Handle); six_state(&motion, &p1Handle);
    assert(rumbles == 1 && motionStarts == 1 && motionReads == 1);

    detach_gamepad(one);
    assert(style(&ids[1]) && read_pad(1).lx == -32767 && !read_pad(0).lx);
    attach_gamepad(one);
    assert(read_pad(1).lx == -32767 && !read_pad(0).lx);

    [two.extendedGamepad.leftThumbstick setValueForXAxis:0 yAxis:0]; refresh_gamepads();
    [two.extendedGamepad.leftThumbstick setValueForXAxis:-1 yAxis:0]; refresh_gamepads();
    /* On 2P -> 1P Odyssey tries to pair two single Joy-Cons before opening controller support.
       Full-key pads cannot be paired. Both must remain independent until the applet selects 1P. */
    uint32_t (*mergeJoy)(const uint32_t *, const uint32_t *) =
        runtime_input_override("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_");
    assert(mergeJoy && mergeJoy(&ids[0], &ids[1]) == 0x4b2cau);
    assert(style(&ids[1]) && !read_pad(0).lx && read_pad(1).lx == -32767);
    supported_styles(7);
    arg.singleMode = 1; /* single mode overrides the player-count range */
    show_controller_support(&result, &arg);
    assert(result.players == 1 && !style(&ids[1]) && read_pad(0).lx == -32767);
    drain_main_queue(); assert(lastFeedbackCandidate == one); /* one was reconnected most recently */
    arg.singleMode = 0; arg.min = arg.max = 1;
    show_controller_support_arg(&arg);
    assert(!style(&ids[1]));
    supported_ids(ids, 1); arg.min = arg.max = 2;
    assert(show_controller_support(&result, &arg) != 0 && !style(&ids[1]));
    supported_ids(ids, 2); detach_gamepad(two);
    assert(show_controller_support(&result, &arg) != 0 && result.players == 1);
    attach_gamepad(two); assert(!style(&ids[1]));
    assert(!show_controller_support(&result, &arg) && style(&ids[1]));
    supported_ids(ids, 1);
    [two.extendedGamepad.leftThumbstick setValueForXAxis:-1 yAxis:0]; refresh_gamepads();
    assert(!style(&ids[1]) && read_pad(0).lx == -32767);
    handheld = 1; supported_ids(&handheldID, 1); supported_styles(3);
    arg.min = arg.max = 2; show_controller_support(&result, &arg);
    assert(result.players == 1 && result.selected == handheldID);
    assert(style(&handheldID) == 2 && !style(&ids[0]) && !style(&ids[1]));
    puts("pass: two-player Npad routing");
    return 0;
}}
