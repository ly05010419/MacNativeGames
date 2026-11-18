/* Mouse mode (shared/README.md): the mouse's movement as the right stick and the gyro, its buttons as physical buttons
   17-21, for the games whose runtime/gamepad_game.h sets GAMEPAD_MOUSE_MODE. Included by runtime/gamepad_core.h. The
   helpers at the end (mouse_physical, mouse_reset_locked, mouse_stick_merge_locked, mouse_start) do nothing in a game
   without it, whose mouse belongs to the keyboard layout only (runtime/input.c). */
#ifndef GAMEPAD_MOUSE_H
#define GAMEPAD_MOUSE_H
#if GAMEPAD_MOUSE_MODE
#include "mouse_look.h"       // mouse mode: movement as the right stick and the gyro

/* ---- Mouse mode (Mac, the settings panel; in mouse.txt) ----
   The window (runtime/frontend.m) holds the pointer while mouse mode is on and the window has the keyboard, and sends
   the mouse's buttons and movement here. Buttons go to the keyboard and mouse layout (runtime/input.c, keyboard_map.h:
   left attack, right draw the bow with left to shoot, middle lock on), which the settings panel's key bindings change;
   they are also physical buttons 17-21 of the controller layout, for mouse lines in controls.txt. Movement goes to
   runtime/mouse_look.h, ticked every 10 ms by a timer while mouse mode is on: the right stick (the stronger of it and a
   controller's) and, unless TOTK_MOUSE_GYRO=0 or the game has no six-axis device (TOTK_GYRO=0), the gyro, which the
   mouse then has to itself (runtime_motion_mouse). Settings: TOTK_MOUSE_SENSITIVITY 1-100 (50; else the settings
   panel's, in mouse-sensitivity.txt), TOTK_MOUSE_INVERT_Y=1,
   TOTK_MOUSE_COUNTERWEIGHT, TOTK_MOUSE_DECAY, TOTK_MOUSE_MIN_DECAY 0-100 (20, 18, 6). */
static _Atomic int mouseMode;
static unsigned mouseButtons;                  /* main thread; bit i = mouse button i (0 left, 1 right, 2 middle, 3, 4) */
static MouseLook mouseLook;                    /* gamepadLock */
static MouseLookSettings mouseSettings;        /* gamepadLock */
static int32_t mouseRX, mouseRY;               /* gamepadLock; the stick shown by the last tick */
static int mouseGyro;                          /* the mouse feeds the gyro in this mouse mode */
static dispatch_source_t mouseTimer;
static int mouseTicksByHand;                   /* tests/mouse/probe.m drives mouse_tick itself */
static void (*mouseHook)(void);                /* the window: mouse mode changed */

static void mouse_tick(void) {
    double gyro[3], acc[3]; int32_t rx, ry;
    pthread_mutex_lock(&gamepadLock);
    mouse_look_point(&mouseLook, gamepad_game_mouse_points());   /* the game's pointing (gamepad_game.h), mouse_look.h */
    mouse_look_tick(&mouseLook, &mouseSettings, &rx, &ry, gyro, acc);
    mouseRX = rx; mouseRY = ry;
    int feed = mouseGyro;
    pthread_mutex_unlock(&gamepadLock);
    if (feed) runtime_motion_feed_mouse(acc, gyro, runtime_now_ns());
}

/* The settings panel's sensitivity row (key 1, runtime/settings_panel.h): TOTK_MOUSE_SENSITIVITY decides when set (the
   row greyed out), else mouse-sensitivity.txt, else 50. A pick is saved and applies at once, also in the middle of a turn. */
static _Atomic int mouseSensitivity;           /* 0: mouse-sensitivity.txt not read yet */
int runtime_mouse_sensitivity_locked(void) { return getenv("TOTK_MOUSE_SENSITIVITY") != NULL; }
int runtime_mouse_sensitivity(void) {
    if (runtime_mouse_sensitivity_locked()) return mouse_look_setting(getenv("TOTK_MOUSE_SENSITIVITY"), 1, 100, 50);
    int s = atomic_load(&mouseSensitivity);
    if (s) return s;
    char path[4096], word[16] = {0};
    s = 50;
    if (setting_read("mouse-sensitivity.txt", word, sizeof word, path, sizeof path)) {
        size_t n = strlen(word);
        while (n && (word[n - 1] == '\n' || word[n - 1] == '\r' || word[n - 1] == ' ' || word[n - 1] == '\t')) word[--n] = 0;
        s = mouse_look_setting(word, 1, 100, 0);
        if (!s) { fprintf(stderr, "[totk-input] %s: not a number from 1 to 100; sensitivity 50\n", path); s = 50; }
    }
    int none = 0;
    atomic_compare_exchange_strong(&mouseSensitivity, &none, s);
    return atomic_load(&mouseSensitivity);
}
void runtime_mouse_sensitivity_set(int sensitivity) {
    if (runtime_mouse_sensitivity_locked() || sensitivity < 1 || sensitivity > 100) return;
    atomic_store(&mouseSensitivity, sensitivity);
    char word[8];
    snprintf(word, sizeof word, "%d", sensitivity);
    setting_write("mouse-sensitivity.txt", word, "mouse sensitivity");
    pthread_mutex_lock(&gamepadLock);
    mouseSettings.sensitivity = sensitivity;
    pthread_mutex_unlock(&gamepadLock);
    fprintf(stderr, "[totk-input] mouse sensitivity %d (settings)\n", sensitivity);
}

static void mouse_apply(int on) {   /* start or stop the ticks and the gyro takeover */
    if (on) {
        const char *g = getenv("TOTK_MOUSE_GYRO");
        MouseLookSettings settings = mouse_look_settings(NULL, getenv("TOTK_MOUSE_INVERT_Y"),
                                                         getenv("TOTK_MOUSE_COUNTERWEIGHT"), getenv("TOTK_MOUSE_DECAY"), getenv("TOTK_MOUSE_MIN_DECAY"));
        settings.sensitivity = runtime_mouse_sensitivity();
        int gyro = runtime_gyro_enabled() && !(g && !strcmp(g, "0"));
        pthread_mutex_lock(&gamepadLock);
        mouseSettings = settings; mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0; mouseGyro = gyro;
        pthread_mutex_unlock(&gamepadLock);
        runtime_motion_mouse(gyro);
        fprintf(stderr, "[totk-input] mouse mode on: sensitivity %d%s, counterweight %d, decay %d, minimum decay %d, gyro %s\n",
                settings.sensitivity, settings.invert_y ? " (Y inverted)" : "", settings.counterweight, settings.decay, settings.min_decay,
                gyro ? "from the mouse" : "not from the mouse");
        if (!mouseTicksByHand && !mouseTimer) {
            mouseTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_queue_create("totk.mouse", DISPATCH_QUEUE_SERIAL));
            dispatch_source_set_timer(mouseTimer, dispatch_time(DISPATCH_TIME_NOW, MOUSE_LOOK_TICK_NS), MOUSE_LOOK_TICK_NS, 1000000);
            dispatch_source_set_event_handler(mouseTimer, ^{ mouse_tick(); });
            dispatch_resume(mouseTimer);
        }
    } else {
        if (mouseTimer) { dispatch_source_cancel(mouseTimer); mouseTimer = nil; }
        pthread_mutex_lock(&gamepadLock);
        mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0; mouseGyro = 0;
        pthread_mutex_unlock(&gamepadLock);
        runtime_motion_mouse(0);
        fprintf(stderr, "[totk-input] mouse mode off\n");
    }
    if (mouseHook) dispatch_async(dispatch_get_main_queue(), ^{ mouseHook(); });
}

static void load_mouse_mode(void) {
    char path[4096], word[16] = {0};
    if (!setting_read("mouse.txt", word, sizeof word, path, sizeof path)) return;
    int on = !strcmp(word, "on\n") || !strcmp(word, "on");
    if (!on && strcmp(word, "off\n") && strcmp(word, "off")) fprintf(stderr, "[totk-input] %s: not on or off; mouse mode off\n", path);
    if (on != atomic_load(&mouseMode)) { atomic_store(&mouseMode, on); mouse_apply(on); }
}

int runtime_mouse_enabled(void) { return atomic_load(&mouseMode); }
void runtime_mouse_set_hook(void (*hook)(void)) { mouseHook = hook; }

/* The settings panel (key 1, frontend.m). */
int runtime_mouse_set(int on) {
    on = !!on;
    if (on == atomic_load(&mouseMode)) return on;
    atomic_store(&mouseMode, on);
    gamepad_game_mouse_mode(on);
    setting_write("mouse.txt", on ? "on" : "off", "mouse mode");
    if (!on) dispatch_async(dispatch_get_main_queue(), ^{ runtime_mouse_release(); });
    mouse_apply(on);
    return on;
}

void runtime_mouse_move(double dx, double dy) {   /* the window, while it holds the pointer */
    if (!atomic_load(&mouseMode)) return;
    if (fabs(dx) + fabs(dy) >= 1) gamepad_game_mouse_moved();
    pthread_mutex_lock(&gamepadLock);
    mouse_look_move(&mouseLook, dx, dy, &mouseSettings);
    pthread_mutex_unlock(&gamepadLock);
}

void runtime_mouse_button(int button, int down) {   /* main thread */
    if (button < 0 || button >= CONTROLLER_MOUSE_BUTTONS || (down && !atomic_load(&mouseMode))) return;
    unsigned bit = 1u << button, next = down ? mouseButtons | bit : mouseButtons & ~bit;
    if (next == mouseButtons) return;
    mouseButtons = next;
    runtime_input_mouse(button, down);
    refresh_gamepads();
}

void runtime_mouse_release(void) {   /* main thread: the window let the pointer go (focus lost, text entry, mouse mode off) */
    pthread_mutex_lock(&gamepadLock);
    mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0;
    pthread_mutex_unlock(&gamepadLock);
    if (mouseButtons) {
        for (int i = 0; i < CONTROLLER_MOUSE_BUTTONS; i++) if (mouseButtons >> i & 1) runtime_input_mouse(i, 0);
        mouseButtons = 0; refresh_gamepads();
    }
}

/* For gamepad.m: the mouse's buttons as physical buttons (mouse lines in controls.txt), its stick, and the start. */
static uint64_t mouse_physical(void) { return (uint64_t)mouseButtons << CONTROLLER_MOUSE_FIRST; }   /* main thread */
static void mouse_reset_locked(void) { mouse_look_reset(&mouseLook); mouseRX = mouseRY = 0; }      /* gamepadLock */
static void mouse_stick_merge_locked(RuntimeGamepadState *out) {                                    /* gamepadLock */
    out->rx = stronger_axis(out->rx, mouseRX); out->ry = stronger_axis(out->ry, mouseRY);
}
static void mouse_start(void) { load_mouse_mode(); gamepad_game_mouse_mode(runtime_mouse_enabled()); }
#else
static inline uint64_t mouse_physical(void) { return 0; }
static inline void mouse_reset_locked(void) {}
static inline void mouse_stick_merge_locked(RuntimeGamepadState *out) { (void)out; }
static inline void mouse_start(void) {}
#endif
#endif
