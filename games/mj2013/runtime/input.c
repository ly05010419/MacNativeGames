/* The player's input as the game's hid shared memory needs it (hb_hid.c reads hb_input_snapshot): the Mac keyboard as
   the console's USB keyboard and as a controller (keyboard_layout.h), the mouse or a finger as the touch screen, and the
   mouse as the console's mouse. Scripted input (TOTK_INPUT_SCRIPT / TOTK_INPUT_FILE) is the shared input_file.h, with
   this game's own commands: "tap <x> <y>" (a touch of 80 ms at game pixels 1280x720), "touch <x> <y> <ms>",
   "drag <x0> <y0> <x1> <y1> <ms>". */
#include "runtime.h"
#include "keyboard_layout.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t keys[128];                       /* Mac key codes held */
static int touch_active; static float touch_x, touch_y;
static int mouse_buttons; static float mouse_x, mouse_y; static int mouse_seen;

void runtime_input_key(unsigned code, int down) { if (code < 128) { pthread_mutex_lock(&lock); keys[code] = down != 0; pthread_mutex_unlock(&lock); } }
void runtime_input_key_synthetic(unsigned code, int down) { runtime_input_key(code, down); }
void runtime_input_touch(int active, float x, float y) { pthread_mutex_lock(&lock); touch_active = active; touch_x = x; touch_y = y; pthread_mutex_unlock(&lock); }
void runtime_input_mouse(float x, float y, int buttons) { pthread_mutex_lock(&lock); mouse_x = x; mouse_y = y; mouse_buttons = buttons; mouse_seen = 1; pthread_mutex_unlock(&lock); }
void runtime_input_clear(void) { pthread_mutex_lock(&lock); memset(keys, 0, sizeof keys); touch_active = 0; mouse_buttons = 0; pthread_mutex_unlock(&lock); }

void hb_input_snapshot(HbInput *o) {
    memset(o, 0, sizeof *o);
    pthread_mutex_lock(&lock);
    for (unsigned code = 0; code < 128; code++) if (keys[code]) keyboard_layout_apply(code, o);
    o->touch = touch_active; o->touch_x = touch_x; o->touch_y = touch_y;
    o->mouse_x = mouse_x; o->mouse_y = mouse_y; o->mouse_buttons = mouse_buttons; o->mouse_connected = mouse_seen;
    pthread_mutex_unlock(&lock);
}

/* ---------- scripted input (shared/runtime/input_file.h) ---------- */
void hb_heap_find(const char *text);   /* svc_memory.c */
static void touch_for(float x, float y, unsigned ms) { runtime_input_touch(1, x, y); usleep(ms * 1000); runtime_input_touch(0, x, y); }
static int input_game_command(char *line) {
    float x, y, x1, y1; unsigned ms; int used = 0;
    if (sscanf(line, "tap %f %f %n", &x, &y, &used) == 2 && used && !line[used]) { fprintf(stderr, "[totk-input] file: tap %.0f %.0f\n", x, y); touch_for(x, y, 80); return 1; }
    if (sscanf(line, "touch %f %f %u %n", &x, &y, &ms, &used) == 3 && used && !line[used] && ms <= 10000) { fprintf(stderr, "[totk-input] file: touch %.0f %.0f %u ms\n", x, y, ms); touch_for(x, y, ms); return 1; }
    if (sscanf(line, "drag %f %f %f %f %u %n", &x, &y, &x1, &y1, &ms, &used) == 5 && used && !line[used] && ms <= 10000) {
        unsigned steps = ms / 16 ? ms / 16 : 1;
        for (unsigned i = 0; i <= steps; i++) { runtime_input_touch(1, x + (x1 - x) * i / steps, y + (y1 - y) * i / steps); usleep(16000); }
        runtime_input_touch(0, x1, y1); return 1;
    }
    if (sscanf(line, "mouse %f %f %u %n", &x, &y, &ms, &used) == 3 && used && !line[used]) { runtime_input_mouse(x, y, (int)ms); return 1; }
    if (!strcmp(line, "hbregs\n")) { void hb_guest_regs(void); hb_guest_regs(); return 1; }   /* diagnostics: the guest main thread's registers */
    if (!strncmp(line, "hbdword ", 8)) { void hb_heap_dword(uint32_t v); hb_heap_dword((uint32_t)strtoul(line + 8, NULL, 16)); return 1; }
    if (!strncmp(line, "hbfind ", 7)) { line[strcspn(line, "\r\n")] = 0; hb_heap_find(line + 7); return 1; }   /* diagnostics: text in the game's heap */
    return 0;
}
static int input_game_flag(const char *name, int v) { (void)name; (void)v; return 0; }
/* What the shared command stream reaches that a software-drawn game has no part for. */
int runtime_survey_command(const char *line) { (void)line; return 0; }
int runtime_hid_feedback_flag(const char *name, int v) { (void)name; (void)v; return 0; }
__attribute__((weak)) void runtime_capture_press(void);   /* defined in hb_display.c */
void runtime_swkbd_selftest(void) { fprintf(stderr, "[totk-input] kbtest: no text entry in this game\n"); }
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_frame_step_pin(double ms) { (void)ms; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int v) { fprintf(stderr, "[totk-input] flag %s=%d: unknown here\n", name, v); }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_shot_at(long frames, const char *path);   /* hb_display.c */
void dm_gfx_window_shot(const char *path) { dm_gfx_shot_at(1, path); }
void runtime_front_hud_shot(const char *path) { (void)path; }
#include "input_file.h"
