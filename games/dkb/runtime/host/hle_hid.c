// nn::hid (controller): on the real device shared memory is read through the hid service; the host takes the current state written by the front end (Mac keyboard / controller, iOS virtual gamepad / controller).
// Generic, game-independent version: player 1 is always connected, as the handheld controller (Npad id 0x20) with DM_OPMODE=0 (handheld), otherwise as the 1P full-key controller (id 0);
// players 2..8 are full-key controllers on No2..No8 (ids 1..7) while the front end reports a controller for them (dm_pad_players; the Mac gives each physical controller a player
// of its own, front_mac.m). A player whose npad the game does not support (SetSupportedNpadIdType) is not reported: in a one-player game it feeds player 1, otherwise it is not used.
// Six-axis sensor: one per connected controller (two for handheld), at rest lying flat, player 1's shaken side to side while the front end holds DM_BTN_SHAKE (see the six-axis section below).
// Vibration follows the player assignments through hle_haptics.c. Mouse and keyboard: not connected; the controller applet reports the connected players with player 1 selected.
#include "hle.h"
#include "hle_haptics.h"
#include "dm_input.h"
#include "front.h"
#include <stdatomic.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include "motion_fusion.h"

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3, STYLE_JOYRIGHT = 1u << 4 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3, ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
// Stick direction pseudo-buttons (NpadButton 16..23): derived from the stick values, threshold about half travel
enum { STICK_THRESH = 0x4000 };

// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight share one layout (0x28)
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, _pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

// Front-end publishing: the keyboard / touch path (dm_input_set), the scripted path (script_set: DM_KEYS and DM_CTL) and player 1's physical controller path (dm_pad_set) are ORed;
// each further player has a controller path of its own (dm_pad_set_player). Press edges are latched until the game's next read so short presses are not lost. Scripted presses have
// their own path because a connected controller publishes its whole state on every change (a DualShock 4's sticks change all the time), which overwrote them on the shared one;
// the separate paths also keep scripted input out of DM_REC, which records the physical ones (rec_read)
static _Atomic uint64_t g_btn, g_axes, g_latch, g_sc_btn, g_sc_axes, g_sc_latch, g_pad_btn[DM_PLAYERS], g_pad_axes[DM_PLAYERS], g_pad_latch[DM_PLAYERS];
static _Atomic int64_t g_sampling = 1 << 16;   // starts high so the history entries below the newest keep positive sampling numbers
static uint64_t pack(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48;
}
// Shake (DM_BTN_SHAKE on any player 1 path): whole side-to-side cycles on the guest clock, so the controller always comes back to rest. A press starts a shake (or continues one still
// finishing), holding keeps shaking, and a release lets the current cycle finish: a tap of one frame is still one whole shake
#define SHAKE_HZ 6.0
static const uint64_t SHAKE_CYCLE_NS = (uint64_t)(1e9 / SHAKE_HZ);
static _Atomic uint64_t g_shake_t0, g_shake_end;   // the current shake's start and end; end = UINT64_MAX while held
static _Atomic int g_shake_on;
static void shake_note(void) {
    int on = ((atomic_load(&g_btn) | atomic_load(&g_sc_btn) | atomic_load(&g_pad_btn[0])) & DM_BTN_SHAKE) != 0;
    if (atomic_exchange(&g_shake_on, on) == on) return;
    uint64_t t = guest_ns();
    if (on) { if (t >= atomic_load(&g_shake_end)) atomic_store(&g_shake_t0, t); atomic_store(&g_shake_end, UINT64_MAX); }
    else { uint64_t t0 = atomic_load(&g_shake_t0); atomic_store(&g_shake_end, t0 + ((t - t0) / SHAKE_CYCLE_NS + 1) * SHAKE_CYCLE_NS); }
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_btn, buttons); atomic_fetch_or(&g_latch, buttons & ~old); atomic_store(&g_axes, pack(lx, ly, rx, ry));
    shake_note();
}
void dm_pad_set_player(int p, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (p < 0 || p >= DM_PLAYERS) return;
    uint64_t old = atomic_exchange(&g_pad_btn[p], buttons); atomic_fetch_or(&g_pad_latch[p], buttons & ~old); atomic_store(&g_pad_axes[p], pack(lx, ly, rx, ry));
    if (!p) shake_note();
}
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_pad_set_player(0, buttons, lx, ly, rx, ry); }
static void script_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_sc_btn, buttons); atomic_fetch_or(&g_sc_latch, buttons & ~old); atomic_store(&g_sc_axes, pack(lx, ly, rx, ry));
    shake_note();
}
uint64_t dm_input_samples(void) { return (uint64_t)atomic_load(&g_sampling); }
// DM_KEYS scripted input (for tests): "seconds:key[+key][~hold seconds][*count/interval seconds],...", e.g. "8:A,12:A,15:Down~0.5,16:A+B,40:A*80/2" (A every 2 s from 40 s on, 80 times). Key names A B X Y LS RS L R ZL ZR (plus the sticks LLeft LUp LRight LDown and RLeft RUp RRight RDown) Plus Minus Left Up Right Down, left stick LLeft LUp LRight LDown,
//   L<x>/<y> and R<x>/<y> = a stick tilted part way (each -1 to 1, y up: a cursor that a full tilt sweeps past its target), T<x>x<y> = a touch at normalized picture coordinates (origin top left) for the hold time, and Shake = the controller shaken for the hold time (at least one whole shake);
//   held 0.15 s by default, at most KEY_STEPS steps (counted after repeats are expanded). Goes through player 1's scripted path (script_set), ORed with keyboard / touch and player 1's controller, none overrides another. Times are counted from host start.
//   An f before the time means a presented frame number ("f7600:A", and the repeat interval counts frames too): when the machine is busy seconds drift from the game's progress, frame numbers do not (used for the Zelda BotW opening route)
//   An f before the hold counts presented frames too ("f7600:A~f12"): held until 12 frames after the step's frame, and a step that begins by then follows without a release in between (the buttons
//   go straight to its state, a held touch moves instead of lifting): DM_REC writes a run as such steps, one state after the other, and a list of only such steps is applied at the
//   game's reads (keys_sync) instead of by keys_main. Whitespace and newlines separate steps as commas do
typedef struct { double at, hold; uint64_t btn; int32_t lx, ly, rx, ry; int frame, fhold, tap; float tx, ty; } key_step;   // tap: a touch at (tx, ty), normalized; fhold: hold in frames
#define KEY_STEPS (1 << 20)
static key_step *g_steps; static int g_nsteps, g_cap_steps;
static int step_add(const key_step *st) {
    if (g_nsteps >= KEY_STEPS) return 0;
    if (g_nsteps == g_cap_steps) { int cap = g_cap_steps ? g_cap_steps * 2 : 64; key_step *n = realloc(g_steps, (size_t)cap * sizeof *n); if (!n) return 0; g_steps = n; g_cap_steps = cap; }
    g_steps[g_nsteps++] = *st; return 1;
}
static void *keys_main(void *arg) {
    // touching: the previous step handed its finger over to this one; released: the frame the previous step released in (a frame step presses in a later frame, so the game reads the
    // release between two presses even when the replay runs late and the next step is already due: two taps of A stay two presses, not one long one)
    (void)arg; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); int touching = 0; int64_t released = -1;
    for (int i = 0; i < g_nsteps; i++) {
        const key_step *s = &g_steps[i];
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
                   if (s->frame ? (double)dm_guest_frame() >= s->at && (int64_t)dm_guest_frame() > released : el >= s->at) break; usleep(5000); }
        LOG("DM_KEYS: %s%.2f press %#llx (hold %.2f %s)", s->frame ? "frame " : "", s->at, (unsigned long long)s->btn, s->hold, s->fhold ? "frames" : "s");
        if (s->tap) { dm_touch t = {0, s->tx, s->ty}; dm_touch_script(&t, 1); } else if (touching) dm_touch_script(NULL, 0);
        script_set(s->btn, s->lx, s->ly, s->rx, s->ry);
        uint64_t end = 0;
        if (s->fhold) { end = (s->frame ? (uint64_t)s->at : dm_guest_frame()) + (uint64_t)s->hold; while (dm_guest_frame() < end) usleep(5000); }
        else usleep((useconds_t)(s->hold * 1e6));
        // Hand over without a release only to a step that begins where this hold ends (DM_REC's consecutive states), not to one that is merely due by now: a gap in the list is a release
        int next = s->fhold && i + 1 < g_nsteps && g_steps[i + 1].frame && g_steps[i + 1].at <= (double)end;
        touching = next && s->tap;
        if (!next) { script_set(0, 0, 0, 0, 0); if (s->tap) dm_touch_script(NULL, 0); released = (int64_t)dm_guest_frame(); }
    }
    return NULL;
}
// A list of frame steps that are all held in frames (what DM_REC writes) is not played by keys_main, whose thread presses within 5 ms after a present, before or after the game's read
// of that frame: player 1's reads (fill, the touch screen, gestures) call keys_sync first, which applies on the reading thread every step due in the frame the read is in. Every read
// in frame N then sees the state stamped on frame N, as DM_REC stamps a state with the frame of the reads that saw it (rec_read). A list with a step in seconds goes through keys_main
static _Atomic int g_sync_on; static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sync_next, g_sync_held;   // the next step to press; whether g_steps[g_sync_next - 1] is held
static int keys_framed(void) { for (int i = 0; i < g_nsteps; i++) if (!g_steps[i].frame || !g_steps[i].fhold) return 0; return g_nsteps > 0; }
static void keys_sync(void) {
    if (!atomic_load_explicit(&g_sync_on, memory_order_acquire)) return;
    pthread_mutex_lock(&g_sync_mu);
    uint64_t f = dm_guest_frame();
    for (;;) {
        if (g_sync_held) {
            const key_step *s = &g_steps[g_sync_next - 1]; uint64_t end = (uint64_t)s->at + (uint64_t)s->hold;
            if (end > f) break;
            if (g_sync_next < g_nsteps && g_steps[g_sync_next].at <= (double)end) {   // the next step begins where this hold ends: no release (buttons go to its state, a finger moves)
                const key_step *n = &g_steps[g_sync_next++];
                if (n->tap) { dm_touch t = {0, n->tx, n->ty}; dm_touch_script(&t, 1); } else if (s->tap) dm_touch_script(NULL, 0);
                script_set(n->btn, n->lx, n->ly, n->rx, n->ry); continue;
            }
            script_set(0, 0, 0, 0, 0); if (s->tap) dm_touch_script(NULL, 0);   // let go (a hold of 0 frames is pressed and let go before one read: the press latch hands it to that read)
            g_sync_held = 0; continue;
        }
        if (g_sync_next >= g_nsteps) { atomic_store(&g_sync_on, 0); LOG("DM_KEYS: the %d frame steps were played, the last let go at frame %llu", g_nsteps, (unsigned long long)f); break; }
        const key_step *s = &g_steps[g_sync_next];
        if (s->at > (double)f) break;
        if (s->tap) { dm_touch t = {0, s->tx, s->ty}; dm_touch_script(&t, 1); }
        script_set(s->btn, s->lx, s->ly, s->rx, s->ry); g_sync_next++; g_sync_held = 1;
    }
    pthread_mutex_unlock(&g_sync_mu);
}
// The key names of NpadButton bits 0..15 (dm_input.h): DM_KEYS and DM_CTL read them, DM_REC writes them
static const char *const key_names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
// The keys of one step, "key[+key]" (the hold and repeat already cut off), into st
static void parse_keys(char *spec, key_step *st) {
    for (char *k = spec; k && *k;) { char *plus = strchr(k, '+'); if (plus) *plus = 0;
        int found = 0; for (int b = 0; b < 16; b++) if (!strcasecmp(k, key_names[b])) { st->btn |= 1ull << b; found = 1; }
        // left stick fully pushed: LLeft LUp LRight LDown (for games that move on the map by stick only, such as Super Mario 3D World)
        static const char *const sticks[4] = {"LLeft", "LUp", "LRight", "LDown"};
        static const int sx[4] = {-32767, 0, 32767, 0}, sy[4] = {0, 32767, 0, -32767};
        for (int d = 0; d < 4; d++) if (!strcasecmp(k, sticks[d])) { if (sx[d]) st->lx = sx[d]; if (sy[d]) st->ly = sy[d]; found = 1; }
        // right stick the same way: RLeft RUp RRight RDown (camera; Super Mario Odyssey's first hint asks for it)
        static const char *const rsticks[4] = {"RLeft", "RUp", "RRight", "RDown"};
        for (int d = 0; d < 4; d++) if (!strcasecmp(k, rsticks[d])) { if (sx[d]) st->rx = sx[d]; if (sy[d]) st->ry = sy[d]; found = 1; }
        // a stick tilted part way: L<x>/<y> or R<x>/<y>, each from -1 to 1 (y up), e.g. "L-0.6/-0.4". OCTOPATH TRAVELER II's traveler selection opens a traveler's card only while
        // its quill cursor rests on them; a fully tilted stick swept the cursor past every traveler (a tilt of 0.3 is inside its dead zone)
        float fx, fy; if ((k[0] == 'L' || k[0] == 'R') && strchr(k, '/') && sscanf(k + 1, "%f/%f", &fx, &fy) == 2 && fabsf(fx) <= 1 && fabsf(fy) <= 1) {
            int32_t vx = (int32_t)lrintf(fx * 32767), vy = (int32_t)lrintf(fy * 32767);
            if (k[0] == 'L') { st->lx = vx; st->ly = vy; } else { st->rx = vx; st->ry = vy; } found = 1; }
        // a touch: T<x>x<y> with x, y normalized on the picture, origin top left ("f3000:T0.31x0.62", Super Mario Galaxy's file select in handheld mode)
        float tx, ty; if ((k[0] == 'T' || k[0] == 't') && sscanf(k + 1, "%fx%f", &tx, &ty) == 2) { st->tap = 1; st->tx = tx; st->ty = ty; found = 1; }
        if (!strcasecmp(k, "Shake")) { st->btn |= DM_BTN_SHAKE; found = 1; }   // the six-axis sensor shaken (Super Mario Odyssey's cap throw)
        if (!found) LOG("[RED] DM_KEYS unknown key name: %s", k);
        k = plus ? plus + 1 : NULL; }
}
static int step_cmp(const void *a, const void *b) { double x = ((const key_step *)a)->at, y = ((const key_step *)b)->at; return (x > y) - (x < y); }
// A DM_KEYS list into g_steps, in time order
static void keys_parse(const char *spec) {
    char *buf = strdup(spec), *save = NULL; if (!buf) return;
    int full = 0;
    for (char *tok = strtok_r(buf, ", \t\r\n", &save); tok && !full; tok = strtok_r(NULL, ", \t\r\n", &save)) {
        char *c = strchr(tok, ':'); if (!c) { LOG("[RED] DM_KEYS step lacks 'seconds:': %s", tok); continue; }
        *c = 0; int fr = (tok[0] == 'f' || tok[0] == 'F'); key_step st = {atof(tok + fr), 0.15, 0}; st.frame = fr;
        int rep = 1; double every = 0;
        char *r = strchr(c + 1, '*'); if (r) { *r = 0; rep = atoi(r + 1); char *sl = strchr(r + 1, '/'); every = sl ? atof(sl + 1) : 1; if (rep < 1) rep = 1; }
        char *h = strchr(c + 1, '~'); if (h) { *h = 0; st.fhold = h[1] == 'f' || h[1] == 'F'; st.hold = atof(h + 1 + st.fhold); }
        parse_keys(c + 1, &st);
        for (int i = 0; i < rep && !full; i++) { key_step one = st; one.at = st.at + i * every; if (!step_add(&one)) { LOG("[RED] DM_KEYS: more than %d steps, the rest is dropped", KEY_STEPS); full = 1; } }
    }
    free(buf);
    // Steps run in time order, so repeated groups interleave ("290:A*200/3,291:Left*200/3,291.5:A*200/3" = A, Left, A every 3 s): they used to run in list order, one group after the
    // other, and a group listed after a long repeat waited for all of it. A stable sort keeps the listed order of steps at the same time; times and frame numbers are not comparable, so a list
    // that mixes them keeps its listed order
    int mixed = 0; for (int i = 1; i < g_nsteps; i++) mixed |= g_steps[i].frame != g_steps[0].frame;
    if (!mixed && g_nsteps > 1) mergesort(g_steps, (size_t)g_nsteps, sizeof *g_steps, step_cmp);   // stable (BSD libc), and not quadratic for long interleaved repeats
}
// DM_KEYS=@<file>: the steps are read from the file (a DM_REC recording as it is). An environment value is capped by ARG_MAX (1 MiB for arguments and environment together on macOS,
// about 40,000 recorded steps), and iOS's env.txt takes one line per switch; a relative path is from the working directory (Documents on iOS)
static char *keys_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *text = NULL; size_t len = 0, cap = 0, n;
    do { if (len + 65536 + 1 > cap) { cap = cap ? cap * 2 : 1 << 20; char *t = realloc(text, cap); if (!t) { free(text); fclose(f); return NULL; } text = t; }
         n = fread(text + len, 1, cap - len - 1, f); len += n; } while (n);
    fclose(f); text[len] = 0; return text;
}
static void ctl_start(void);
static void rec_start(void);
void hid_keys_init(void) {
    ctl_start();
    rec_start();
    const char *e = getenv("DM_KEYS"); if (!e || !*e) return;
    if (e[0] == '@') {
        char *text = keys_file(e + 1); if (!text) { LOG("[RED] DM_KEYS: cannot read %s", e + 1); return; }
        keys_parse(text); free(text);
    } else keys_parse(e);
    if (keys_framed()) { atomic_store(&g_sync_on, 1); LOG("DM_KEYS: %d steps, all in frames: applied at the game's input reads of their frames", g_nsteps); return; }
    pthread_t th; if (g_nsteps && !pthread_create(&th, NULL, keys_main, NULL)) { pthread_detach(th); LOG("DM_KEYS: %d steps", g_nsteps); }
}

// DM_CTL=<file> (Mac, for driving a game step by step from a shell): the host reads the lines appended to the file (polled every 100 ms) and acts on each at once:
//   "key[+key][~hold]" presses as a DM_KEYS step does (hold 0.15 s by default), "shot <path>" saves the next presented frame as a PNG (dm_gfx_shot_at), "passdump <n>" dumps the
//   render passes of the next n frames as DM_NVN_PASSDUMP does (to stderr, the run's log), "texdump <path with %d>" saves the colour targets of the next frame as DM_NVN_TEXDUMP does,
//   "env KEY=VALUE" sets a switch read at the time of use (DM_NVN_SNAPRAW for the next texdump), "cvarf <main offset> <value>" sets an Unreal float console variable
//   (hle_fpslock.c dm_ctl_cvar_f32; "cvari" an int one), "peek <address> <count>" logs that many floats of guest memory, "memdump <address> <bytes> <path>" saves guest memory
//   to a file (a buffer texture's storage, which the pass dump's tbuf lines print, holds what the GPU wrote: buffer textures are views of the pool), "texlist <NVN target>" logs the
//   registered textures of that target (2 = 3D) with storage address and block-linear layout (nvn_mtl.m dm_gfx_texlist), "texraw <serial> <path>"
//   saves a host texture's level 0 raw, every slice (a cube's six faces, a 3D texture's depth slices) (nvn_mtl.m dm_gfx_texraw), "drag <x0> <y0> <x1> <y1> [seconds=0.3]" moves one finger in a straight line between the two normalized picture
//   points over that time and lifts it (the touch screen's pans and swipes, hle_touch.c gesture_sample), "dump" logs what each guest thread waits on and the graphics wait points
//   (crash_dump_now, as the iOS app's DM_LOG_PORT dump; scripts/regress.py asks for it before stopping a hung game), "#…" is ignored.
//   Look at a screen, decide, press, look again: menus whose timing moves from run to run (loading) need no blind timed script
static void *ctl_main(void *arg) {
    const char *path = arg; long off = 0; char line[1100];
    for (;;) {
        FILE *f = fopen(path, "r");
        if (f && !fseek(f, off, SEEK_SET)) {
            while (fgets(line, sizeof line, f)) {
                size_t n = strlen(line); if (!n || line[n - 1] != '\n') break;   // a line still being written: read it next time
                off += (long)n; line[--n] = 0; if (n && line[n - 1] == '\r') line[--n] = 0;
                if (!n || line[0] == '#') continue;
                if (!strcmp(line, "dump")) { crash_dump_now("DM_CTL dump"); continue; }
                if (!strncmp(line, "shot ", 5)) { dm_gfx_shot_at(1, line + 5); LOG("DM_CTL: screenshot of the next frame → %s", line + 5); continue; }
                if (!strncmp(line, "texlist ", 8)) { void dm_gfx_texlist(int target); dm_gfx_texlist(atoi(line + 8)); continue; }
                if (!strncmp(line, "texraw ", 7)) { void dm_gfx_texraw(uint64_t serial, const char *path); char *e2; uint64_t s = strtoull(line + 7, &e2, 10); while (*e2 == ' ') e2++; dm_gfx_texraw(s, e2); continue; }
                if (!strncmp(line, "peek ", 5)) { void dm_ctl_peek(uint64_t a, int n); char *e2; uint64_t a = strtoull(line + 5, &e2, 16); dm_ctl_peek(a, (int)strtol(e2, NULL, 0)); continue; }
                if (!strncmp(line, "memdump ", 8)) { void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path); char *e2, *e3; uint64_t a = strtoull(line + 8, &e2, 16), n = strtoull(e2, &e3, 0);
                                                     while (*e3 == ' ') e3++; dm_ctl_memdump(a, n, e3); continue; }
                if (!strncmp(line, "cvari ", 6)) { void dm_ctl_cvar_i32(uint64_t off, int32_t v); char *e2; uint64_t off = strtoull(line + 6, &e2, 16); dm_ctl_cvar_i32(off, (int32_t)strtol(e2, NULL, 0)); continue; }
                if (!strncmp(line, "cvarf ", 6)) { void dm_ctl_cvar_f32(uint64_t off, float v); char *e2; uint64_t off = strtoull(line + 6, &e2, 16); dm_ctl_cvar_f32(off, strtof(e2, NULL)); continue; }
                if (!strncmp(line, "env ", 4)) { char *eq = strchr(line + 4, '='); if (eq) { *eq = 0; setenv(line + 4, eq + 1, 1); LOG("DM_CTL: env %s=%s", line + 4, eq + 1); } continue; }
                if (!strncmp(line, "texdump ", 8)) { void dm_gfx_texdump_next(const char *path); dm_gfx_texdump_next(line + 8); LOG("DM_CTL: colour targets of the next frame → %s (as DM_NVN_TEXDUMP)", line + 8); continue; }
                if (!strncmp(line, "passdump ", 9)) { void dm_gfx_passdump_next(long n); dm_gfx_passdump_next(atol(line + 9)); LOG("DM_CTL: passes of the next %ld frames (as DM_NVN_PASSDUMP)", atol(line + 9)); continue; }
                if (!strncmp(line, "drag ", 5)) {   // one finger, a step every 8 ms
                    float x0, y0, x1, y1, s = 0.3f; if (sscanf(line + 5, "%f %f %f %f %f", &x0, &y0, &x1, &y1, &s) < 4) { LOG("[RED] DM_CTL drag wants x0 y0 x1 y1 [seconds]"); continue; }
                    int n = (int)(s / 0.008f); if (n < 2) n = 2;
                    LOG("DM_CTL: drag (%.3f,%.3f) → (%.3f,%.3f) over %.2f s", x0, y0, x1, y1, s);
                    for (int i = 0; i <= n; i++) { dm_touch t = {0, x0 + (x1 - x0) * (float)i / (float)n, y0 + (y1 - y0) * (float)i / (float)n}; dm_touch_script(&t, 1); usleep((useconds_t)(s * 1e6f / (float)n)); }
                    dm_touch_script(NULL, 0); continue;
                }
                key_step st = {0, 0.15, 0}; char *h = strchr(line, '~'); if (h) { *h = 0; st.hold = atof(h + 1); }
                parse_keys(line, &st);
                LOG("DM_CTL: press %#llx (hold %.2f s)", (unsigned long long)st.btn, st.hold);
                if (st.tap) { dm_touch t = {0, st.tx, st.ty}; dm_touch_script(&t, 1); }
                script_set(st.btn, st.lx, st.ly, st.rx, st.ry); usleep((useconds_t)(st.hold * 1e6)); script_set(0, 0, 0, 0, 0);
                if (st.tap) dm_touch_script(NULL, 0);
            }
        }
        if (f) fclose(f);
        usleep(100000);
    }
    return NULL;
}
static void ctl_start(void) {
    const char *e = getenv("DM_CTL"); if (!e || !*e) return;
    pthread_t th; if (!pthread_create(&th, NULL, ctl_main, strdup(e))) { pthread_detach(th); LOG("DM_CTL: reading key presses and screenshot requests from %s", e); }
}

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : NPAD_NO1; }
uint32_t hle_hid_connected_npad(void) { return connected_id(); }   // nfp's NFC device sits on player 1's controller (hle_sdk16.c)
// Player n is on No n in handheld mode too, where No1 stays empty: games take Handheld and No1 both for player 1, so a second controller on No1 would act as player 1 again
uint32_t dm_player_npad(int p) { return p <= 0 ? connected_id() : (uint32_t)p; }
// The players with a controller (bit p = player p + 1, from the front end's dm_pad_players; player 1 always) and the npads the game supports (bit id for No1..No8, bit 8
// Handheld, bit 9 Other: all of them until SetSupportedNpadIdType, as the hid service starts)
static _Atomic uint32_t g_players = 1, g_ids_supported = 0x3ff;
static uint32_t id_bit(uint32_t id) { return id < DM_PLAYERS ? 1u << id : id == NPAD_HANDHELD ? 1u << 8 : id == 0x10 ? 1u << 9 : 0; }
static int reported(int p) { return p == 0 || (atomic_load(&g_players) >> p & 1 && atomic_load(&g_ids_supported) & id_bit((uint32_t)p)); }
// Players with a controller whose npad the game does not support. A one-player game (no npad past No1 supported) gets them as player 1, so any controller plays it; in a game for
// fewer players than are connected (a fifth controller in a four-player game) they are not used, as on the Switch, where the system does not connect them
static uint32_t unsupported_players(void) {
    uint32_t m = 0; for (int p = 1; p < DM_PLAYERS; p++) if (atomic_load(&g_players) >> p & 1 && !reported(p)) m |= 1u << p;
    return m;
}
static int one_player(void) { return !(atomic_load(&g_ids_supported) & ((1u << DM_PLAYERS) - 2)); }
static uint32_t merged_players(void) { return one_player() ? unsupported_players() : 0; }
static int player_of(uint32_t id) {   // the reported player on npad id; -1 when none
    if (id == connected_id()) return 0;
    return id > 0 && id < DM_PLAYERS && reported((int)id) ? (int)id : -1;
}
static uint32_t style_for_player(uint32_t id, int p) { return p < 0 ? 0 : id == NPAD_HANDHELD ? STYLE_HANDHELD : STYLE_FULLKEY; }
static uint32_t style_of(uint32_t id) { return style_for_player(id, player_of(id)); }
static int32_t ax(uint64_t v, int i) { return (int16_t)(v >> (16 * i)); }
static int32_t pick(int32_t a, int32_t b) { return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }   // whichever of two sources is deflected more

// Style update events bound per npad (BindNpadStyleSetUpdateEvent, No1..No8 by id): signalled when a player's controller comes or goes, as the hid
// service does. The front end reports from its own thread, where an event cannot be bound, so only a bound one is signalled (one the game destroyed is not bound any more)
static void *_Atomic g_style_ev[DM_PLAYERS];
static void style_changed(uint32_t id) { void *ev = id < DM_PLAYERS ? atomic_load(&g_style_ev[id]) : NULL; if (ev) hle_signal_bound_system_event(ev); }
static void note_merged(void) {   // logs the players whose npad the game does not support, when that changes
    static _Atomic uint32_t last; uint32_t m = unsupported_players(), one = (uint32_t)one_player();
    if (atomic_exchange(&last, m | one << 31) == (m | one << 31)) return;
    for (int p = 1; p < DM_PLAYERS; p++) if (m >> p & 1) {
        if (one) LOG("hid: player %d's controller feeds player 1: the game supports one player (no npad %d)", p + 1, p);
        else LOG("hid: player %d's controller is not used: the game supports no npad %d", p + 1, p);
    }
}
void dm_pad_players(uint32_t mask) {
    mask = (mask | 1u) & ((1u << DM_PLAYERS) - 1);
    uint32_t old = atomic_exchange(&g_players, mask);
    for (int p = 1; p < DM_PLAYERS; p++) if ((old ^ mask) >> p & 1) {
        if (!(mask >> p & 1)) dm_pad_set_player(p, 0, 0, 0, 0, 0);   // a controller that went away leaves nothing pressed
        if (atomic_load(&g_ids_supported) & id_bit((uint32_t)p)) style_changed((uint32_t)p);
    }
    note_merged();
}

// DM_REC=<file>: player 1's physical input (keyboard / touch, its controller and the controllers merged into it, as fill reads them) written as DM_KEYS steps, so a run plays again
// with DM_KEYS=@<file> from the same save and build: a bug report's path without the game's data, or a regress route recorded instead of written. DM_KEYS and DM_CTL have
// paths of their own (script_set, dm_touch_script) and are never recorded. Each step is one state of player 1, from the presented frame it began to the frame it ended:
// "f<frame>:<keys>~f<frames>," on a line of its own, the idle states between them left out. Frames, not seconds: under DM_DETCLOCK the guest clock and the game follow the
// presents, while seconds from host start drift with load. A state is stamped with the frame of the game's reads that saw it (rec_read, called by fill for player 1), not with the
// moment it arrived, and the replay applies it at the reads of that frame (keys_sync): frame N of the replay reads what frame N of the recording read. Within one frame the
// buttons of all its reads are ORed (a press that came and went between two reads reached the game through the press latch) and the sticks and the touch are the last read's.
// Sticks in hundredths of full tilt (L<x>/<y>, R<x>/<y>, what DM_KEYS reads back exactly), recorded again when an axis moves REC_STICK_STEP hundredths or leaves or comes back to the
// centre; the touchscreen as its first finger (T<x>x<y> in thousandths of the picture), recorded again when it moves REC_TOUCH_STEP thousandths, taken at player 1's controller reads
// (a touch that came and went between two of them counts once). The reading thread takes a mutex, compares and formats into memory; a writer thread appends that to the file every
// REC_FLUSH_MS (no file I/O on an input or guest thread). Off, a read costs one branch
enum { REC_STICK_STEP = 2, REC_TOUCH_STEP = 5, REC_FLUSH_MS = 250 };
typedef struct { uint64_t btn; int32_t q[4]; int touch; int32_t tx, ty; } rec_state;   // q: the sticks in hundredths; tx, ty: thousandths
static _Atomic int g_rec_on;
static pthread_mutex_t g_rec_mu = PTHREAD_MUTEX_INITIALIZER, g_rec_io = PTHREAD_MUTEX_INITIALIZER;
static rec_state g_rec_cur; static uint64_t g_rec_from;   // the open state and the frame it began
static rec_state g_rec_frame; static uint64_t g_rec_at; static int g_rec_have;   // the state the reads of frame g_rec_at saw so far (g_rec_have: there were reads)
static int g_rec_touch, g_rec_tlatch; static float g_rec_tx, g_rec_ty;   // the front end's first finger (rec_touch); tlatch: put down since the last read
static char *g_rec_buf; static size_t g_rec_len, g_rec_cap;   // text not written yet
static FILE *g_rec_file;
extern void (*_Atomic dm_touch_observer)(const dm_touch *t, int n);   // hle_touch.c: called by dm_touch_set (the front ends), not by dm_touch_script
static int rec_idle(const rec_state *s) { return !s->btn && !s->q[0] && !s->q[1] && !s->q[2] && !s->q[3] && !s->touch; }
static int rec_same(const rec_state *a, const rec_state *b) {   // within the recording's steps
    if (a->btn != b->btn || a->touch != b->touch) return 0;
    if (a->touch && (abs(a->tx - b->tx) >= REC_TOUCH_STEP || abs(a->ty - b->ty) >= REC_TOUCH_STEP)) return 0;
    for (int i = 0; i < 4; i++) if (!a->q[i] != !b->q[i] || abs(a->q[i] - b->q[i]) >= REC_STICK_STEP) return 0;
    return 1;
}
// One state as DM_KEYS keys ("A+ZR+L0.50/-0.25+T0.250x0.750"); returns the length
static int rec_keys(const rec_state *s, char *out, size_t cap) {
    int n = 0;
    #define PUT(fmt, ...) do { int w_ = snprintf(out + n, cap - (size_t)n, "%s" fmt, n ? "+" : "", ##__VA_ARGS__); if (w_ > 0 && (size_t)(n + w_) < cap) n += w_; } while (0)
    for (int b = 0; b < 16; b++) if (s->btn >> b & 1) PUT("%s", key_names[b]);
    if (s->btn & DM_BTN_SHAKE) PUT("Shake");
    if (s->q[0] || s->q[1]) PUT("L%.2f/%.2f", s->q[0] / 100.0, s->q[1] / 100.0);
    if (s->q[2] || s->q[3]) PUT("R%.2f/%.2f", s->q[2] / 100.0, s->q[3] / 100.0);
    if (s->touch) PUT("T%.3fx%.3f", s->tx / 1000.0, s->ty / 1000.0);   // fixed decimals: "0x…" would read as a hexadecimal number
    #undef PUT
    return n;
}
static void rec_step(const rec_state *s, uint64_t from, uint64_t to) {   // appends one step to the text buffer (g_rec_mu held)
    char keys[160], line[200]; rec_keys(s, keys, sizeof keys);
    int n = snprintf(line, sizeof line, "f%llu:%s~f%llu,\n", (unsigned long long)from, keys, (unsigned long long)(to - from));
    if (n <= 0 || (size_t)n >= sizeof line) return;
    if (g_rec_len + (size_t)n > g_rec_cap) { size_t cap = g_rec_cap ? g_rec_cap * 2 : 4096; while (cap < g_rec_len + (size_t)n) cap *= 2; char *b = realloc(g_rec_buf, cap); if (!b) return; g_rec_buf = b; g_rec_cap = cap; }
    memcpy(g_rec_buf + g_rec_len, line, (size_t)n); g_rec_len += (size_t)n;
}
static void rec_frame_done(void) {   // the reads of frame g_rec_at are over: its state ends the open one when it differs (g_rec_mu held)
    if (!g_rec_have) return;
    if (!rec_same(&g_rec_frame, &g_rec_cur)) {
        if (!rec_idle(&g_rec_cur)) rec_step(&g_rec_cur, g_rec_from, g_rec_at);
        g_rec_cur = g_rec_frame; g_rec_from = g_rec_at;
    }
    g_rec_have = 0;
}
// A read of player 1 (fill): btn and a are what it composed from the physical sources, before the scripted path joins
static void rec_read(uint64_t btn, const int32_t a[4]) {
    rec_state s = {btn};
    for (int i = 0; i < 4; i++) { long q = lrint(a[i] * 100.0 / DM_STICK_MAX); s.q[i] = (int32_t)(q < -100 ? -100 : q > 100 ? 100 : q); }
    pthread_mutex_lock(&g_rec_mu);
    uint64_t f = dm_guest_frame();
    if (g_rec_have && f != g_rec_at) rec_frame_done();
    if ((s.touch = g_rec_touch || g_rec_tlatch)) { s.tx = (int32_t)lrintf(g_rec_tx * 1000); s.ty = (int32_t)lrintf(g_rec_ty * 1000); }
    g_rec_tlatch = 0;
    if (!g_rec_have) { g_rec_frame = s; g_rec_at = f; g_rec_have = 1; }
    else { s.btn |= g_rec_frame.btn; g_rec_frame = s; }   // buttons over all the frame's reads, the sticks and the touch as the last read saw them
    pthread_mutex_unlock(&g_rec_mu);
}
static void rec_touch(const dm_touch *t, int n) {
    pthread_mutex_lock(&g_rec_mu); g_rec_touch = n > 0; if (n > 0) { g_rec_tx = t[0].x; g_rec_ty = t[0].y; g_rec_tlatch = 1; } pthread_mutex_unlock(&g_rec_mu);
}
static void rec_flush(void) {   // the text so far to the file, outside the input lock
    pthread_mutex_lock(&g_rec_io);
    pthread_mutex_lock(&g_rec_mu); char *b = g_rec_buf; size_t n = g_rec_len; g_rec_buf = NULL; g_rec_len = g_rec_cap = 0; pthread_mutex_unlock(&g_rec_mu);
    if (n && g_rec_file) { fwrite(b, 1, n, g_rec_file); fflush(g_rec_file); }
    free(b);
    pthread_mutex_unlock(&g_rec_io);
}
// At exit (exit, SIGTERM through dm_host_shutdown): the open state ends at the current frame, or after the last frame read when that is later (a state its reads saw lasts through
// them), and everything goes to the file. A kill loses at most the last REC_FLUSH_MS
void dm_rec_finish(void) {
    if (!atomic_load(&g_rec_on)) return;
    pthread_mutex_lock(&g_rec_mu);
    uint64_t last = g_rec_have ? g_rec_at + 1 : 0; rec_frame_done();
    uint64_t f = dm_guest_frame(); if (f < last) f = last;
    if (!rec_idle(&g_rec_cur)) rec_step(&g_rec_cur, g_rec_from, f);
    g_rec_cur = (rec_state){0}; g_rec_from = f;
    pthread_mutex_unlock(&g_rec_mu);
    rec_flush();
}
static void *rec_main(void *arg) { (void)arg; for (;;) { usleep(REC_FLUSH_MS * 1000); rec_flush(); } return NULL; }
static int rec_open(const char *path) {   // a new recording (the file starts empty: one file, one run's timeline)
    if (!(g_rec_file = fopen(path, "w"))) { LOG("[RED] DM_REC: cannot write %s", path); return 0; }
    atomic_store(&dm_touch_observer, rec_touch);
    atomic_store(&g_rec_on, 1);
    return 1;
}
static void rec_start(void) {
    const char *e = getenv("DM_REC"); if (!e || !*e || !rec_open(e)) return;
    pthread_t th; if (!pthread_create(&th, NULL, rec_main, NULL)) pthread_detach(th);
    atexit(dm_rec_finish);
    LOG("DM_REC: recording player 1's input to %s as DM_KEYS steps in presented frames", e);
}

static void fill(NpadState *s, uint32_t id, uint32_t style) {
    memset(s, 0, sizeof *s);
    s->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    int pl = player_of(id);
    if (!(style_for_player(id, pl) & style)) return;   // no controller of this id / style: attributes = 0
    // Player 1: keyboard / touch, script, its controller and the players merged into it; another player: its own controller path
    uint32_t src = pl ? 1u << pl : 1u | merged_players();
    uint64_t b = 0; int32_t a[4] = {0};
    if (!pl) {
        keys_sync();   // DM_KEYS frame steps due in this frame, before the read
        b = atomic_load(&g_btn) | atomic_exchange(&g_latch, 0);
        uint64_t k = atomic_load(&g_axes); for (int i = 0; i < 4; i++) a[i] = ax(k, i);
    }
    for (int p = 0; p < DM_PLAYERS; p++) if (src >> p & 1) {
        b |= atomic_load(&g_pad_btn[p]) | atomic_exchange(&g_pad_latch[p], 0);
        uint64_t v = atomic_load(&g_pad_axes[p]); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(v, i));
    }
    if (!pl) {   // the physical sources so far (DM_REC records them, DM_BTN_SHAKE only from player 1's own controller and the keyboard, as shake_note), then the scripted path
        if (atomic_load_explicit(&g_rec_on, memory_order_relaxed)) rec_read((b & 0xffffull) | ((atomic_load(&g_btn) | atomic_load(&g_pad_btn[0])) & DM_BTN_SHAKE), a);
        b |= atomic_load(&g_sc_btn) | atomic_exchange(&g_sc_latch, 0);
        uint64_t c = atomic_load(&g_sc_axes); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(c, i));
    }
    s->lx = a[0]; s->ly = a[1]; s->rx = a[2]; s->ry = a[3];
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED | ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
}
// The cheat VM's key mask (hle_cheats.c "all keys of the mask held"): the OR of every player's controller buttons
// (the keyboard / touch source's bits included) plus the eight stick-direction pseudo buttons of the merged axes —
// one flat bitset, not exactly any one npad read (fill() publishes per-npad states with attributes, merges and
// latches), and nothing here consumes a latch or bumps the sampling: the game's own reads keep them both. Mask bits:
// the buttons 0..15 in NpadButton order and the sticks' directions 16..23; SL / SR (bits 24 / 25) can never fire, no
// input source of this host reports them
uint64_t dm_cheats_keys(void) {
    uint64_t b = 0; int32_t a[4] = {0};
    b |= atomic_load(&g_btn);
    for (int p = 0; p < DM_PLAYERS; p++) {
        b |= atomic_load(&g_pad_btn[p]);
        uint64_t v = atomic_load(&g_pad_axes[p]); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(v, i));
    }
    uint64_t k = atomic_load(&g_axes); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(k, i));
    b &= 0xffffull;
    if (a[0] < -STICK_THRESH) b |= 1u << 16;
    if (a[1] > STICK_THRESH) b |= 1u << 17;
    if (a[0] > STICK_THRESH) b |= 1u << 18;
    if (a[1] < -STICK_THRESH) b |= 1u << 19;
    if (a[2] < -STICK_THRESH) b |= 1u << 20;
    if (a[3] > STICK_THRESH) b |= 1u << 21;
    if (a[2] > STICK_THRESH) b |= 1u << 22;
    if (a[3] < -STICK_THRESH) b |= 1u << 23;
    return b;
}
// GetNpadStates(out, n, id): newest first, n entries: the current state repeated with consecutive sampling numbers. The host keeps no history, but on hardware the
// 16-entry history is always full, and games read all of it whatever count comes back: Super Mario Odyssey (main+0x93fc20) polls every controller slot each frame, which advances
// the shared sampling number by about 16 per frame, and with only entry 0 written neither stick did anything in play (the camera did not turn at its first hint) while buttons
// and menus (the stick's direction bits) worked; with every entry written the camera turns
static int states(NpadState *out, int n, uint32_t id, uint32_t style) {
    if (n <= 0) return 0;
    fill(out, id, style);
    for (int i = 1; i < n; i++) { out[i] = out[0]; out[i].sampling = out[0].sampling - i; }
    return n;
}

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "connect to the hid service; the host takes input from the front-end state", void, h_hid_InitNpad, void) {}
// Supported npad ids: players 2..8 are reported only on supported npads; a one-player game ({No1, Handheld}) gets every controller as player 1 (merged_players); a four-player
// game ({No1..No4, Handheld}) gets players 1..4, and a fifth controller is not used
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "supported controller ids; the host records them and reports further players only on supported ids", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) {
    uint32_t m = 0; for (uint64_t i = 0; ids && i < n; i++) m |= id_bit(ids[i]);
    uint32_t was = 0; for (int p = 1; p < DM_PLAYERS; p++) was |= (uint32_t)reported(p) << p;
    if (atomic_exchange(&g_ids_supported, m) == m) return;
    for (int p = 0; p < DM_PLAYERS; p++) dm_haptics_clear(p);
    LOG("hid supported controller ids %#x (bit n: No n+1, bit 8 Handheld, bit 9 Other)", m);
    for (int p = 1; p < DM_PLAYERS; p++) if ((uint32_t)reported(p) != (was >> p & 1)) style_changed((uint32_t)p);
    note_merged();
}
static uint32_t g_styles_supported = 0xffffffffu;
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "supported controller styles; the host records them and picks the reported style from them", void, h_hid_SetStyles, uint32_t styles) {
    if (styles != g_styles_supported) LOG("hid supported controller styles %#x", styles); g_styles_supported = styles; }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "controller style query; follows DM_OPMODE", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "full-key controller states", int, h_hid_StatesFullKey, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "handheld controller states", int, h_hid_StatesHandheld, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", "dual Joy-Con states (not connected on the host)", int, h_hid_StatesJoyDual, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "left Joy-Con states (not connected on the host)", int, h_hid_StatesJoyLeft, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", "right Joy-Con states (not connected on the host)", int, h_hid_StatesJoyRight, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYRIGHT); }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "disconnect a controller; ignored by the host", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
static uint32_t g_hold_type;
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con hold orientation; the host only records it", void, h_hid_HoldType, int t) { g_hold_type = (uint32_t)t; }
HLE_FORCE("_ZN2nn3hid20GetNpadJoyAssignmentERKj", "Joy-Con assignment: dual", int, h_hid_GetJoyAssign, const uint32_t *id) { (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKj", "Joy-Con single assignment; ignored by the host", void, h_hid_JoySingle, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid18SwapNpadAssignmentERKjS2_", "swap controller ids; ignored by the host", void, h_hid_Swap, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid21StartLrAssignmentModeEv", "L/R assignment mode; ignored by the host", void, h_hid_LrStart, void) {}
HLE_FORCE("_ZN2nn3hid20StopLrAssignmentModeEv", "L/R assignment mode; ignored by the host", void, h_hid_LrStop, void) {}
// Controller applet (system UI): report the connected players right away, with player 1's controller selected. ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; };
// ControllerSupportArg starts with { int8 playerCountMin, playerCountMax }: the count is capped at the maximum. Fewer players than the minimum: on a Switch the applet stays open
// until enough controllers connect or the player closes it, and the SDK's ShowControllerSupportCore (Mario Kart 8 Deluxe 4.0.0 sdk+0x24eeb0) turns the applet's result into
// 0 success, 2 → 2202-3102, anything else → ResultControllerSupportCanceled (2202-3101). The host has nothing to wait for, so it answers as the player closing it. An OK with
// fewer players is an answer a Switch never gives: Mario Kart 8 Deluxe asks for two in 2P and opened the applet again every few frames on it (GitHub issue #550)
#define RES_CTRL_CANCELED (202u | 3101u << 9)
static Result ctrl_support(int32_t *info, const int8_t *arg) {
    int count = 0; for (int p = 0; p < DM_PLAYERS; p++) count += reported(p);
    int canceled = arg && arg[0] > count;
    if (arg) LOG("hid controller applet: %d to %d players wanted, %d connected%s", arg[0], arg[1], count, canceled ? ": too few, closed as the player would (canceled)" : "");
    if (arg && arg[1] > 0 && count > arg[1]) count = arg[1];
    if (info) { info[0] = count; info[1] = (int32_t)connected_id(); }
    return canceled ? RES_CTRL_CANCELED : RES_OK;
}
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "controller applet; the host reports the connected players right away (canceled with fewer than the minimum)",
          Result, h_hid_ShowCtrl, int32_t *info, const int8_t *arg) {
    DM_APPLET("controller support");
    return ctrl_support(info, arg);
}
// Six-axis sensor (accelerometer + gyroscope) of the connected controller. SixAxisSensorState (0x60): acceleration in G, angular velocity in revolutions / s, angle in revolutions,
// direction = the controller's three axes. At rest it lies flat: gravity (0, 0, -1), no rotation, the identity direction. While shaking (DM_BTN_SHAKE: the iPhone shaken, DmGamepad.m;
// Space on the Mac; DM_KEYS Shake) it moves side to side, x acceleration ±SHAKE_G with a yaw of ±SHAKE_REV, in whole cycles that end where they began. Samples are 5 ms apart on the
// guest clock, as the sensor's 200 Hz, and numbered by time, so a history read returns the samples at those times. Super Mario Odyssey reads all 16 entries of GetSixAxisSensorStates
// whatever count comes back (main+0x93fd84), takes the newest entry's acceleration for its swing detection (main+0x626244) and its angular velocity, angle and direction (main+0x5d72d4)
typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9]; uint32_t attributes, _pad; } SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "SixAxisState");
enum { SIX_PERIOD_NS = 5000000 };
#define SHAKE_G 3.0
#define SHAKE_REV 1.5
static int shaking(uint64_t t) { return t >= atomic_load(&g_shake_t0) && t < atomic_load(&g_shake_end); }
// Live motion (dm_motion_set: a controller's gyro and accelerometer, front_mac.m / DmGamepad.m through motion_apple.h; dm_motion_set_switch: the iPhone's own, already in the
// Switch frame, DmGamepad.m): while a player's samples keep arriving (within
// MOTION_STALE_NS on the host clock) their six-axis sensor reports them (motion_fusion.h) instead of lying flat; a shake (DM_BTN_SHAKE) still adds its side-to-side acceleration.
// Every history entry is the newest sample, numbered by time. DM_MOTION=0 ignores live motion; DM_MOTION_AXES / DM_MOTION_ACC_SIGN map Apple's axes (motion_fusion.h);
// DM_MOTION_GAIN sets the tilt correction (default 0.5); DM_MOTION_LOG=1 logs player 1's mapped sample once a second, for checking the axes
enum { MOTION_STALE_NS = 300000000 };
typedef struct { pthread_mutex_t mu; motion_state st; uint64_t last_ns, received_ns, logged; } live_motion;
static live_motion g_motion[DM_PLAYERS] = {[0 ... DM_PLAYERS - 1] = {.mu = PTHREAD_MUTEX_INITIALIZER}};
static live_motion g_hid_motion[DM_PLAYERS] = {[0 ... DM_PLAYERS - 1] = {.mu = PTHREAD_MUTEX_INITIALIZER}};
static live_motion g_phone_motion = {.mu = PTHREAD_MUTEX_INITIALIZER};
static pthread_once_t g_motion_once = PTHREAD_ONCE_INIT;
static motion_map g_motion_map; static float g_motion_gain = 0.5f; static int g_motion_off, g_motion_log, g_phone_mode;
static void motion_env(void) {
    const char *e = getenv("DM_MOTION"); g_motion_off = e && !atoi(e);
    if (!motion_map_parse(&g_motion_map, getenv("DM_MOTION_AXES"))) LOG("[hid] DM_MOTION_AXES='%s' is not like \"x,-z,y\": using x,y,z", getenv("DM_MOTION_AXES"));
    if ((e = getenv("DM_MOTION_ACC_SIGN")) && atof(e) < 0) g_motion_map.acc_sign = -1;
    if ((e = getenv("DM_MOTION_GAIN"))) g_motion_gain = (float)atof(e);
    g_motion_log = (e = getenv("DM_MOTION_LOG")) && atoi(e);
    e = getenv("DM_PHONE_MOTION"); g_phone_mode = !e || !strcmp(e, "auto") ? -1 : !!atoi(e);
}
static void motion_set(int p, const float acc[3], const float rot[3], int source, uint64_t sample_ns) {
    if (p < 0 || p >= DM_PLAYERS || (source == 1 && p)) return;
    pthread_once(&g_motion_once, motion_env); if (g_motion_off) return;
    static motion_map identity = {{0, 1, 2}, {1, 1, 1}, 1, 1};
    // Each transport owns its orientation. HID batches carry three sample times; arrival time controls freshness, never their integration intervals.
    live_motion *m = source == 1 ? &g_phone_motion : source == 2 ? &g_hid_motion[p] : &g_motion[p];
    uint64_t now = now_ns(), t = sample_ns ? sample_ns : now;
    if (!t || t > now || now - t > MOTION_STALE_NS) return;
    pthread_mutex_lock(&m->mu);
    if (!m->last_ns || now - m->received_ns > MOTION_STALE_NS) { motion_reset(&m->st); m->last_ns = 0; }
    if (m->last_ns && t <= m->last_ns) { pthread_mutex_unlock(&m->mu); return; }
    float dt = m->last_ns && t > m->last_ns ? (float)((double)(t - m->last_ns) / 1e9) : 0;
    motion_update(&m->st, source ? &identity : &g_motion_map, acc, rot, dt, g_motion_gain);
    if (g_motion_log && !p && t - m->logged > 1000000000ull) {
        m->logged = t;
        LOG("[hid] motion player 1 (%s): acc %+.2f %+.2f %+.2f G, gyro %+.3f %+.3f %+.3f rev/s (flat and still should read acc 0 0 -1)",
            source == 1 ? "phone" : source == 2 ? "controller HID" : "controller", m->st.acc[0], m->st.acc[1], m->st.acc[2], m->st.gyro[0], m->st.gyro[1], m->st.gyro[2]);
    }
    m->last_ns = t; m->received_ns = now;
    pthread_mutex_unlock(&m->mu);
}
void dm_motion_set(int p, const float acc[3], const float rot[3]) { motion_set(p, acc, rot, 0, 0); }
void dm_motion_set_switch(int p, const float acc[3], const float rot[3]) { motion_set(p, acc, rot, 1, 0); }
void dm_motion_set_hid(int p, const float acc[3], const float rot[3], uint64_t t) { if (t) motion_set(p, acc, rot, 2, t); }
void dm_motion_clear(int p) {
    if (p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_motion[p].mu); g_motion[p].last_ns = 0; pthread_mutex_unlock(&g_motion[p].mu);
}
void dm_motion_clear_hid(int p) {
    if (p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_hid_motion[p].mu); g_hid_motion[p].last_ns = 0; pthread_mutex_unlock(&g_hid_motion[p].mu);
}
static uint64_t motion_copy(live_motion *m, motion_state *out) {   // copies a source while it is fresh; returns its timestamp
    uint64_t t;
    pthread_mutex_lock(&m->mu);
    t = m->last_ns && now_ns() - m->received_ns <= MOTION_STALE_NS ? m->last_ns : 0;
    if (t) *out = m->st;
    pthread_mutex_unlock(&m->mu);
    return t;
}
static int motion_live(int p, motion_state *out) {
    if (p < 0 || p >= DM_PLAYERS) return 0;
    pthread_once(&g_motion_once, motion_env);
    if (!p && g_phone_mode > 0) return motion_copy(&g_phone_motion, out) != 0;
    // Match the button path: in a one-player game, a controller in another slot also drives player 1.
    uint32_t src = p ? 1u << p : 1u | merged_players(); uint64_t newest = 0;
    for (int i = 0; i < DM_PLAYERS; i++) if (src >> i & 1) {
        motion_state st; uint64_t t = motion_copy(&g_motion[i], &st);
        if (!t) t = motion_copy(&g_hid_motion[i], &st);
        if (t > newest) { newest = t; *out = st; }
    }
    if (newest) return 1;
    return !p && g_phone_mode < 0 && motion_copy(&g_phone_motion, out);
}
static void six_sample_live(SixAxisState *s, int64_t samp, const motion_state *ms, int shakes) {
    memset(s, 0, sizeof *s);
    uint64_t t = (uint64_t)samp * SIX_PERIOD_NS;
    s->delta_ns = SIX_PERIOD_NS; s->sampling = samp; s->attributes = 1;
    motion_fill(ms, s->acc, s->gyro, s->angle, s->dir);
    if (shakes && shaking(t)) s->acc[0] += (float)(SHAKE_G * sin(2 * M_PI * SHAKE_HZ * (double)(t - atomic_load(&g_shake_t0)) / 1e9));
}
static void six_sample(SixAxisState *s, int64_t samp, int shakes) {
    memset(s, 0, sizeof *s);
    uint64_t t = (uint64_t)samp * SIX_PERIOD_NS;
    s->delta_ns = SIX_PERIOD_NS; s->sampling = samp; s->attributes = 1;   // IsConnected
    s->acc[2] = -1;
    double yaw = 0;
    if (shakes && shaking(t)) {
        double ph = 2 * M_PI * SHAKE_HZ * (double)(t - atomic_load(&g_shake_t0)) / 1e9;
        s->acc[0] = (float)(SHAKE_G * sin(ph));
        s->gyro[2] = (float)(SHAKE_REV * sin(ph));
        yaw = SHAKE_REV / (2 * M_PI * SHAKE_HZ) * (1 - cos(ph)); s->angle[2] = (float)yaw;
    }
    float c = (float)cos(2 * M_PI * yaw), sn = (float)sin(2 * M_PI * yaw);
    s->dir[0] = c; s->dir[1] = sn; s->dir[3] = -sn; s->dir[4] = c; s->dir[8] = 1;
}
// SixAxisSensorHandle: byte 0 the style's bit number, byte 1 the npad id, byte 2 the device (left / right Joy-Con of a pair). The styles made of two Joy-Cons (handheld, dual) have
// two sensors, the others one. A handle reads as connected while its controller is connected in that style; otherwise its entries are zero (not connected) and the count 0.
// Only player 1's controller shakes (DM_BTN_SHAKE comes from the keyboard, the iPhone and DM_KEYS, all player 1)
static int six_connected(uint32_t h) { return (h & 0xff) < 32 && (style_of(h >> 8 & 0xff) >> (h & 0xff) & 1); }
static int six_states(SixAxisState *out, int n, uint32_t h) {
    if (n <= 0) return 0;
    if (!six_connected(h)) { memset(out, 0, sizeof *out * (size_t)n); return 0; }
    int64_t samp = (int64_t)(guest_ns() / SIX_PERIOD_NS);
    int pl = player_of(h >> 8 & 0xff), shakes = pl == 0;
    motion_state ms;
    if (motion_live(pl, &ms)) { for (int i = 0; i < n; i++) six_sample_live(&out[i], samp - i, &ms, shakes); return n; }
    for (int i = 0; i < n; i++) six_sample(&out[i], samp - i, shakes);
    return n;
}
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "six-axis sensor handles: two for handheld / dual Joy-Cons, one otherwise",
          int, h_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) {
    int k = style & (STYLE_HANDHELD | STYLE_JOYDUAL) ? 2 : 1; if (k > n) k = n;
    for (int i = 0; i < k; i++) h[i] = (uint32_t)(style ? __builtin_ctz(style) : 0) | (*id & 0xff) << 8 | (uint32_t)i << 16;
    return k > 0 ? k : 0;
}
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "six-axis sensor; always sampling on the host", void, h_hid_SixStart, const uint32_t *h) { (void)h; }
// Merged mouse pointer (Nmouse, Switch 2 SDKs; Pokemon Pokopia, main+0x11251dc). The SDK opens the hid service itself in InitializeNmouse, which the host does not serve (abort 0xe15). No device
// reports here: GetNmouseHandles finds none and the merged handle, which exists without a device on the console, has no states. The game skips its mouse code on those answers (main+0x112638c, +0x11262b4)
HLE_FORCE("_ZN2nn3hid14FinalizeNmouseEv", "merged mouse: nothing to release", void, h_hid_NmouseFini, void) {}
HLE_FORCE("_ZN2nn3hid26SetMergedNmouseSourceTypesENS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEE", "merged mouse sources: no device", void, h_hid_NmouseSources, uint32_t types) { (void)types; }
HLE_FORCE("_ZN2nn3hid21GetMergedNmouseHandleEPNS0_12NmouseHandleE", "merged mouse handle: 0, with no states", int, h_hid_NmouseMerged, uint64_t *out) { *out = 0; return 1; }
HLE_FORCE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", "mouse types: none", int, h_hid_NmouseTypes, uint32_t *out, uint64_t h) { (void)h; *out = 0; return 1; }
HLE_FORCE("_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", "mouse states: none", int, h_hid_NmouseStates, int *count, void *out, int n, uint64_t h) { (void)out; (void)n; (void)h; *count = 0; return 1; }

// Joy-Con 2 mouse sensor (JCMS, Switch 2 SDKs; Donkey Kong Bananza). The SDK opens the hid service itself in InitializeJcms, which the host does not serve. A handle is the npad id in
// byte 0 and the JcmsType in byte 1 (sdk hid::utility::MakeJcmsHandle); a JcmsState is 32 bytes, and GetJcmsStates returns 0 while its ring holds no samples, as here: no controller
// of the host has the sensor
HLE_FORCE("_ZN2nn3hid14InitializeJcmsEPvm", "Joy-Con mouse sensor: no hid service; no sensor on the host", void, h_hid_JcmsInit, void *work, uint64_t size) { (void)work; (void)size; }
HLE_FORCE("_ZN2nn3hid12FinalizeJcmsEv", "Joy-Con mouse sensor: nothing to release", void, h_hid_JcmsFini, void) {}
HLE_FORCE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", "Joy-Con mouse sensor handle: npad id | type << 8, as the SDK makes it",
          uint32_t, h_hid_JcmsHandle, const uint32_t *id, const uint32_t *type) { return (*id & 0xff) | (*type & 0xff) << 8; }
HLE_FORCE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: no sensor on the host", void, h_hid_JcmsStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: no sensor on the host", void, h_hid_JcmsStop, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", "Joy-Con mouse sensor states: none (no sensor on the host)",
          int, h_hid_JcmsStates, void *out, int n, const uint32_t *h) { (void)out; (void)n; (void)h; return 0; }
// The Switch 2's pointer on top of those sensors (Nmouse; Fire Emblem Fortune's Weave initializes it at boot with the 1920x1080 screen it maps the pointer to, main+0x650a24). InitializeNmouse opens the
// hid service as InitializeJcms does (its Result aborts: sdk+0x6d5340). No controller of the host has the sensor, so there are no handles, and the calls on a handle have nothing to do
HLE_FORCE("_ZN2nn3hid16InitializeNmouseEPvmii", "Joy-Con mouse pointer: no hid service; no sensor on the host", void, h_hid_NmouseInit, void *work, uint64_t size, int w, int h) { (void)work; (void)size; (void)w; (void)h; }
HLE_FORCE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", "Joy-Con mouse pointer handles: none (no sensor on the host)", int, h_hid_NmouseHandles, void *out, int n, uint32_t id) { (void)out; (void)n; (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid32SetNmouseButtonConfigJoyLeftJcmsERKNS0_29NmouseButtonConfigJoyLeftJcmsE", "Joy-Con mouse pointer: no sensor on the host", void, h_hid_NmouseCfgL, const void *c) { (void)c; }
HLE_FORCE("_ZN2nn3hid33SetNmouseButtonConfigJoyRightJcmsERKNS0_30NmouseButtonConfigJoyRightJcmsE", "Joy-Con mouse pointer: no sensor on the host", void, h_hid_NmouseCfgR, const void *c) { (void)c; }
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "six-axis sensor states: every requested entry, newest first",
          int, h_hid_SixStates, SixAxisState *out, int n, const uint32_t *h) { return six_states(out, n, *h); }
// Vibration handles: byte 0 = SDK style index (FullKey 3, Handheld 4), byte 1 = npad id, byte 2 = motor.
// See libnx hid.c _hidGetVibrationDeviceHandles. Only the styles this host connects are exposed.
static uint32_t vib_targets(uint32_t h) {
    unsigned type = h & 0xff, id = h >> 8 & 0xff, motor = h >> 16 & 0xff;
    if (h >> 24 || motor > 1 || (type != 3 && type != 4)) return 0;
    int p = player_of(id);
    if (style_for_player(id, p) != (1u << (type - 3))) return 0;
    return dm_haptics_available() & (p == 0 ? 1u | merged_players() : 1u << p);
}
static void vib_send(const uint32_t *handles, const dm_vibration *values, int n) {
    if (!handles || !values || n <= 0) return;
    dm_vibration_update updates[DM_PLAYERS * 2];
    uint32_t used = 0; int unrouted = 0;
    for (int i = 0; i < n; i++) {
        uint32_t targets = vib_targets(handles[i]);
        if (!targets) unrouted++;
        for (int p = 0; p < DM_PLAYERS; p++) if (targets >> p & 1) {
            int m = handles[i] >> 16 & 1, k = p * 2 + m;
            updates[k] = (dm_vibration_update){p, m, values[i]}; used |= 1u << k;
        }
    }
    int count = 0;
    for (int k = 0; k < DM_PLAYERS * 2; k++) if (used >> k & 1) updates[count++] = updates[k];
    if (getenv("DM_RUMBLE_LOG")) {   // Count input values before deduplicating motors, including partially routed batches.
        static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
        static uint64_t sends, dropped, t0; static float peak; struct timespec ts;
        pthread_mutex_lock(&log_lock);   // SDK sends can arrive from several game threads.
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = ts.tv_sec; sends += n; dropped += unrouted;
        for (int i = 0; i < n; i++) peak = fmaxf(peak, fmaxf(values[i].amplitude_low, values[i].amplitude_high));
        if (now != t0) { LOG("rumble: %llu values sent, %llu with no output, peak amplitude %.2f, handle %#x", sends, dropped, peak, handles[0]); sends = dropped = 0; peak = 0; t0 = now; }
        pthread_mutex_unlock(&log_lock);
    }
    dm_haptics_send(updates, count);
}
HLE_FORCE("_ZN2nn3hid20IsVibrationPermittedEv", "vibration permission: enabled and an output available", unsigned, h_hid_VibPermit, void) { return dm_haptics_available() != 0; }
// Games can cache handles once at startup, before GameController routes the pad's haptics (Unicorn Overlord kept an empty answer and sent every value to handle 0).
// On Horizon handles are computed from npad id and style (libnx _hidGetVibrationDeviceHandles); vib_send checks for an output at each send. With rumble off they are handed out too,
// as on a Switch whose vibration setting is off (IsVibrationPermitted answers false and nothing is felt), so turning it on while the game runs works in a game that asked only once.
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "vibration devices of the connected controller",
          int, h_hid_VibHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) {
    if (!h || !id || n <= 0 || !(style & style_of(*id)) || !dm_haptics_has_output()) {
        if (getenv("DM_RUMBLE_LOG") && id) LOG("rumble: game asked for vibration handles of npad %u style %#x: none (connected style %#x, %s)", *id, style, style_of(*id), dm_haptics_has_output() ? "output ready" : "no output yet");
        return 0;
    }
    uint32_t base = (*id << 8) | (*id == NPAD_HANDHELD ? 4u : 3u);
    int count = n < 2 ? n : 2;
    for (int i = 0; i < count; i++) h[i] = base | (uint32_t)i << 16;
    if (getenv("DM_RUMBLE_LOG")) LOG("rumble: game asked for vibration handles of npad %u style %#x: %d, first %#x", *id, style, count, h[0]);
    return count;
}
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "vibration: initialize one channel", void, h_hid_InitVib, const uint32_t *h) {
    const dm_vibration idle = {0, 160, 0, 320}; vib_send(h, &idle, 1);
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "vibration: update one channel", void, h_hid_SendVib, const uint32_t *h, const dm_vibration *v) { vib_send(h, v, 1); }
// Values queued by the vibration graph (VibrationNode::Update) reach it from the sdk's internal FlushEnqueuedVibrationValues through the PLT, into the hid service proxy (sm abort). Metroid Dread
// sets handles on its VibrationTargets even when the host reports 0 devices, so values get queued; return success and the flush clears its count as usual
HLE_FORCE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", "vibration batch: update all channels atomically", Result, h_hid_SendVibs, const uint32_t *h, const dm_vibration *v, int n) { vib_send(h, v, n); return RES_OK; }
// Mouse / keyboard: not connected
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "mouse; not provided by the host", void, h_hid_InitMouse, void) {}
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "mouse states: 0 entries", int, h_hid_MouseStates, void *out, int n) { (void)out; (void)n; return 0; }
// Single state (13 Sentinels calls it every frame; without the hook the sdk aborts on its uninitialized mouse resource): MouseState is 0x28 bytes {sampling number i64, x, y, dx, dy, wheel dx, dy, buttons, attributes}; attributes 0 = not connected
HLE_FORCE("_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", "mouse state: not connected (MouseState 0x28 bytes zeroed)", void, h_hid_MouseState, void *s) {
    memset(s, 0, 0x28); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "keyboard; not provided by the host", void, h_hid_InitKbd, void) {}
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "keyboard state: not connected (KeyboardState 0x30 bytes zeroed)", void, h_hid_KbdState, void *s) {
    memset(s, 0, 0x30); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}
// The history form (Monster Hunter Rise): every requested entry, newest first, with consecutive sampling numbers, as GetNpadStates fills them
HLE_FORCE("_ZN2nn3hid17GetKeyboardStatesEPNS0_13KeyboardStateEi", "keyboard state history: not connected (each 0x30-byte entry zeroed)", int, h_hid_KbdStates, uint8_t *s, int n) {
    int64_t top = atomic_fetch_add(&g_sampling, 1) + 1;
    for (int i = 0; i < n; i++) { memset(s + 0x30 * i, 0, 0x30); *(int64_t *)(s + 0x30 * i) = top - i; }
    return n > 0 ? n : 0;
}
// The key code map of the system keyboard layout, which the SDK asks the settings service for (settings::detail::GetKeyCodeMap, an abort without it). DRAGON QUEST VII Reimagined (Unreal Engine
// 4.27, main+0x1f0eb4) reads the keyboard state, then zeroes a 0x1000-byte map and fetches it before translating keys with kpr::GetKeyCode. With no keyboard the map stays as the game made it
HLE_FORCE("_ZN2nn3kpr13GetKeyCodeMapEPNS0_10KeyCodeMapE", "keyboard layout key code map: none (no keyboard; the caller's map is left as it is)", void, h_kpr_Map, void *map) { (void)map; }

// ---------- SDK 12 interfaces (Engage): single GetNpadState, mode queries / setters, touch screen, gestures, six-axis details, controller colors, etc. ----------
// In the original all of these read hid shared memory or go through the hid service; the host has no shared memory and answers from the same front-end state as above. Library functions that only modify the game's own buffers (vibration file parse / generate, ControllerSupportArg SetDefault / SetExplainText) keep running in the SDK
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj", "full-key controller current state", void, h_hid_StateFullKey, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadHandheldStateERKj", "handheld controller current state", void, h_hid_StateHandheld, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyDualStateERKj", "dual Joy-Con current state (not connected on the host)", void, h_hid_StateJoyDual, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_16NpadJoyLeftStateERKj", "left Joy-Con current state (not connected on the host)", void, h_hid_StateJoyLeft, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid12GetNpadStateEPNS0_17NpadJoyRightStateERKj", "right Joy-Con current state (not connected on the host)", void, h_hid_StateJoyRight, NpadState *s, const uint32_t *id) { fill(s, *id, STYLE_JOYRIGHT); }

static uint32_t g_comm_mode, g_hh_mode;
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Joy-Con hold orientation: returns the value that was set", int, h_hid_GetHold, void) { return (int)g_hold_type; }
HLE_FORCE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", "supported controller styles: all", uint32_t, h_hid_GetStyles, void) { return g_styles_supported; }
HLE_FORCE("_ZN2nn3hid24GetNpadCommunicationModeEv", "wireless communication mode: returns the value that was set", int, h_hid_GetComm, void) { return (int)g_comm_mode; }
HLE_FORCE("_ZN2nn3hid24SetNpadCommunicationModeENS0_21NpadCommunicationModeE", "wireless communication mode; the host only records it", void, h_hid_SetComm, int m) { g_comm_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid29GetNpadHandheldActivationModeEv", "handheld activation mode: returns the value that was set", int, h_hid_GetHh, void) { return (int)g_hh_mode; }
HLE_FORCE("_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", "handheld activation mode; the host only records it", void, h_hid_SetHh, int m) { g_hh_mode = (uint32_t)m; }
HLE_FORCE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", "Joy-Con dual assignment; ignored by the host", void, h_hid_JoyDual, const uint32_t *id) { (void)id; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE", "Joy-Con single assignment (left or right given); ignored by the host", void, h_hid_JoySingleT, const uint32_t *id, int t) { (void)id; (void)t; }
HLE_FORCE("_ZN2nn3hid30SetNpadJoyAssignmentModeSingleEPjRKjNS0_17NpadJoyDeviceTypeE", "Joy-Con single assignment (returning the other half); ignored by the host, there is no other half", int, h_hid_JoySingleOut, uint32_t *out, const uint32_t *id, int t) { (void)out; (void)id; (void)t; return 0; }
HLE_FORCE("_ZN2nn3hid23MergeSingleJoyAsDualJoyERKjS2_", "merge single Joy-Cons; ignored by the host, reports success", Result, h_hid_Merge, const uint32_t *a, const uint32_t *b) { (void)a; (void)b; return RES_OK; }
// Bound and signalled once when its controller is connected (the hid service reports the controller already attached at start the same way), and again whenever a player's controller
// comes or goes (dm_pad_players). Link's Awakening reads no controller state until this event has fired: without it GetNpadState was never called and the title's "Press L+R" never saw a press
HLE_FORCE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", "controller style update event: bound, signaled once for a connected controller and on connect / disconnect", void, h_hid_BindStyleEv,
          const uint32_t *id, void *ev, int clear) {
    hle_bind_system_event(ev, clear);
    if (*id < DM_PLAYERS) atomic_store(&g_style_ev[*id], ev);
    if (style_of(*id)) hle_signal_system_event(ev);
}
// The Switch's player LEDs by npad (No1 1, No2 1-2, No3 1-3, No4 1-4, No5 1+4, No6 1+3, No7 1+3+4, No8 2+3; bit 0 = LED 1); Handheld and Other light LED 1 as before
HLE_FORCE("_ZN2nn3hid19GetPlayerLedPatternERKj", "player LED: the Switch's pattern of the npad", uint8_t, h_hid_Led, const uint32_t *id) {
    static const uint8_t led[DM_PLAYERS] = {0x1, 0x3, 0x7, 0xf, 0x9, 0x5, 0xd, 0x6};
    return *id < DM_PLAYERS ? led[*id] : 1;
}
// NpadControllerColor = { main RGBA, button RGBA }: grey body, black buttons
static void ctrl_color(uint32_t *c) { c[0] = 0xff828282u; c[1] = 0xff0f0f0fu; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorERKj", "controller color", Result, h_hid_Color, uint32_t *c, const uint32_t *id) { (void)id; ctrl_color(c); return RES_OK; }
HLE_FORCE("_ZN2nn3hid22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj", "left and right Joy-Con colors", Result, h_hid_Color2, uint32_t *l, uint32_t *r, const uint32_t *id) { (void)id; ctrl_color(l); ctrl_color(r); return RES_OK; }
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", "controller applet (no result wanted); the host answers as the overload above", Result, h_hid_ShowCtrl0, const int8_t *arg) { DM_APPLET("controller support"); return ctrl_support(NULL, arg); }
HLE_FORCE("_ZN2nn3hid24ShowControllerStrapGuideEv", "strap guide applet; not shown by the host", Result, h_hid_Strap, void) { DM_APPLET("controller strap guide"); return RES_OK; }
HLE_FORCE("_ZN2nn3hid24ShowControllerStrapGuideERKNS0_13StrapGuideArgE", "strap guide applet with its argument (SDK 22); not shown by the host", Result, h_hid_StrapArg, const void *arg) { DM_APPLET("controller strap guide"); (void)arg; return RES_OK; }
HLE_FORCE("_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", "controller firmware update applet; not shown by the host", Result, h_hid_FwUpd, const void *arg) { DM_APPLET("controller firmware update"); (void)arg; return RES_OK; }

// Touch screen: TouchScreenState<N> = { int64 sampling number; int32 count; int32 reserved; TouchState[N] }, filled from the front end's touch points (hle_touch.c touch_fill_n:
// the Mac's mouse, the iPhone touches no gamepad control takes, DM_KEYS taps) in handheld mode, as on the Switch, whose screen is out of reach when docked: a game may read touches
// without checking the mode, so in console mode the state stays empty (only the header is written). Super Mario Galaxy's pointer is the touch screen in handheld mode
int touch_fill_n(void *out, int max, uint64_t now_ns);
static void touch_read(void *s, int n) {
    keys_sync();   // DM_KEYS frame steps due in this frame (their touches), before the read
    if (dm_opmode() == 0) { touch_fill_n(s, n, now_ns()); return; }
    int64_t *p = s; p[0] = atomic_fetch_add(&g_sampling, 1) + 1; ((int32_t *)s)[2] = 0; ((int32_t *)s)[3] = 0;
}
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEv", "connect to the hid touch screen; the host takes touch points from the front end", void, h_hid_InitTouch, void) {}
// SDK 18 overload with two ints (Super Mario Galaxy); the SDK's own one opens the hid service and aborts in sm::Initialize
HLE_FORCE("_ZN2nn3hid21InitializeTouchScreenEii", "connect to the hid touch screen (SDK 18 two-argument version); the host takes touch points from the front end", void, h_hid_InitTouch2, int a, int b) { (void)a; (void)b; }
#define TOUCH(N) \
    HLE_FORCE("_ZN2nn3hid19GetTouchScreenStateILm" #N "EEEvPNS0_16TouchScreenStateIXT_EEE", "touch screen current state (touch points only in handheld mode)", void, h_hid_Touch1_##N, void *s) { touch_read(s, N); } \
    HLE_FORCE("_ZN2nn3hid20GetTouchScreenStatesILm" #N "EEEiPNS0_16TouchScreenStateIXT_EEEi", "touch screen states: 1 entry (touch points only in handheld mode)", int, h_hid_TouchN_##N, void *s, int n) { if (n <= 0) return 0; touch_read(s, N); return 1; }
TOUCH(1) TOUCH(2) TOUCH(3) TOUCH(4) TOUCH(5) TOUCH(6) TOUCH(7) TOUCH(8) TOUCH(9) TOUCH(10) TOUCH(11) TOUCH(12) TOUCH(13) TOUCH(14) TOUCH(15) TOUCH(16)
// Gestures: recognized from the same touch points (hle_touch.c gesture_fill), so only in handheld mode too
int gesture_fill(void *out, int n, uint64_t now_ns);
HLE_FORCE("_ZN2nn3hid17InitializeGestureEv", "connect to the hid gesture recognizer; the host recognizes gestures from the touch points", void, h_hid_InitGesture, void) {}
HLE_FORCE("_ZN2nn3hid16GetGestureStatesEPNS0_12GestureStateEi", "gesture states, newest first (handheld mode: recognized from the touch points; console mode: none)", int, h_hid_Gestures, void *out, int n) {
    keys_sync();
    return dm_opmode() == 0 ? gesture_fill(out, n, now_ns()) : 0;
}

// Six-axis details: the current state, at rest unless shaking, fusion off
HLE_FORCE("_ZN2nn3hid17StopSixAxisSensorERKNS0_19SixAxisSensorHandleE", "six-axis sensor; always sampling on the host", void, h_hid_SixStop, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", "six-axis sensor current state", void, h_hid_SixState, SixAxisState *s, const uint32_t *h) { six_states(s, 1, *h); }
HLE_FORCE("_ZN2nn3hid21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE", "six-axis sensor at rest: yes unless player 1's is shaking or a live controller is turning", unsigned, h_hid_SixRest, const uint32_t *h) {
    int pl = player_of(*h >> 8 & 0xff); motion_state ms;
    if (motion_live(pl, &ms)) return fabsf(ms.gyro[0]) + fabsf(ms.gyro[1]) + fabsf(ms.gyro[2]) < 0.01f && !(pl == 0 && shaking(guest_ns()));
    return pl != 0 || !shaking(guest_ns());
}
// The console's own six-axis sensor (SevenSixAxisSensor, the body's orientation in handheld mode; The Legend of Zelda: Breath of the Wild): takes a work buffer and is switched through the hid service. The host provides none: switching succeeds, 0 states
HLE_FORCE("_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm", "console six-axis sensor initialization; not provided by the host", Result, h_hid_SevenInit, void *buf, size_t n) { (void)buf; (void)n; return RES_OK; }
HLE_FORCE("_ZN2nn3hid26FinalizeSevenSixAxisSensorEv", "console six-axis sensor", Result, h_hid_SevenFini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid23StartSevenSixAxisSensorEv", "console six-axis sensor; not provided by the host", Result, h_hid_SevenStart, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid22StopSevenSixAxisSensorEv", "console six-axis sensor", Result, h_hid_SevenStop, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv", "console six-axis sensor timestamp", void, h_hid_SevenReset, void) {}
HLE_FORCE("_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi", "console six-axis sensor states: 0 entries", int, h_hid_SevenStates, void *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv", "console six-axis sensor at rest: yes (Super Mario Odyssey)", unsigned, h_hid_SevenRest, void) { return 1; }
// The development debug pad (Super Mario Odyssey initializes and polls it): never connected, all zero
HLE_FORCE("_ZN2nn3hid18InitializeDebugPadEv", "debug pad; the host has none", void, h_hid_DbgPadInit, void) {}
HLE_FORCE("_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", "debug pad state: not connected (all 0)", void, h_hid_DbgPadState, void *st) { memset(st, 0, 32); }   // sampling number, attributes, buttons, two sticks
// The history form (Hollow Knight: Silksong, Unity 6, main+0x1296c60 every frame; the SDK's own aborts with Result 0x2aca on its uninitialized debug pad resource): every requested entry, newest first
HLE_FORCE("_ZN2nn3hid17GetDebugPadStatesEPNS0_13DebugPadStateEi", "debug pad state history: not connected (each 32-byte entry zeroed)", int, h_hid_DbgPadStates, uint8_t *s, int n) {
    int64_t top = atomic_fetch_add(&g_sampling, 1) + 1;
    for (int i = 0; i < n; i++) { memset(s + 32 * i, 0, 32); *(int64_t *)(s + 32 * i) = top - i; }
    return n > 0 ? n : 0;
}
HLE_FORCE("_ZN2nn3hid25EnableSixAxisSensorFusionERKNS0_19SixAxisSensorHandleEb", "six-axis sensor fusion; ignored by the host", void, h_hid_SixFusionOn, const uint32_t *h, uint32_t on) { (void)h; (void)on; }
HLE_FORCE("_ZN2nn3hid28IsSixAxisSensorFusionEnabledERKNS0_19SixAxisSensorHandleE", "six-axis sensor fusion: off", unsigned, h_hid_SixFusionQ, const uint32_t *h) { (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", "gyroscope zero-drift mode: standard", int, h_hid_GyroGet, const uint32_t *h) { (void)h; return 1; }
HLE_FORCE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", "gyroscope zero-drift mode; ignored by the host", void, h_hid_GyroSet, const uint32_t *h, const int *m) { (void)h; (void)m; }
HLE_FORCE("_ZN2nn3hid32GetSixAxisSensorFusionParametersEPfS1_RKNS0_19SixAxisSensorHandleE", "six-axis sensor fusion parameters: 0", void, h_hid_FusionGet, float *a, float *b, const uint32_t *h) { (void)h; *a = 0; *b = 0; }
HLE_FORCE("_ZN2nn3hid32SetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleEff", "six-axis sensor fusion parameters; ignored by the host", void, h_hid_FusionSet, const uint32_t *h, float a, float b) { (void)h; (void)a; (void)b; }
HLE_FORCE("_ZN2nn3hid34ResetSixAxisSensorFusionParametersERKNS0_19SixAxisSensorHandleE", "six-axis sensor fusion parameters; ignored by the host", void, h_hid_FusionReset, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid41IsFirmwareUpdateAvailableForSixAxisSensorERKNS0_19SixAxisSensorHandleE", "six-axis sensor firmware update: none", unsigned, h_hid_SixFw, const uint32_t *h) { (void)h; return 0; }
// GetActual returns the sanitized guest value accepted by the host, before output-device adaptation.
HLE_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "vibration: linear resonant actuator, left or right", void, h_hid_VibInfo, uint32_t *info, const uint32_t *h) {
    int on = h && vib_targets(*h); info[0] = on ? 1 : 0; info[1] = on ? 1 + (*h >> 16 & 1) : 0;
}
// The SDK's own call reaches the hid service (a null read in hid::detail::IsVibrationDeviceMounted; Super Mario Odyssey asks for each handle of a controller).
// On Horizon a device is mounted when the npad's connected controller has that style and an actuator on that side: here, when the channel has an output, as GetVibrationDeviceInfo reports it
HLE_FORCE("_ZN2nn3hid24IsVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", "vibration: device mounted when the channel has an output", unsigned, h_hid_VibMounted, const uint32_t *h) {
    return h && vib_targets(*h);
}
HLE_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "vibration: last accepted channel value", void, h_hid_VibActual, dm_vibration *v, const uint32_t *h) {
    uint32_t targets = h ? vib_targets(*h) : 0;
    *v = targets ? dm_haptics_actual(__builtin_ctz(targets), *h >> 16 & 1) : (dm_vibration){0, 160, 0, 320};
}

// Poké Ball Plus (Palma, Pokémon Sword / Shield, SDK 7.3.2): the host has none. The SDK's own calls reach the hid service (SetPalmaBoostMode, the first, at start-up: a null read in
// hid::detail::SetPalmaBoostMode, sdk+0x238eb0). No npad reports the Palma style, so the game never gets a connection handle; every call on a handle fails as for a handle that is not
// connected (hid 202-3302, ResultPalmaInvalidHandle in yuzu's hid errors), and the operation complete event is never bound
#define RES_PALMA_INVALID_HANDLE (202u | 3302u << 9)
HLE_FORCE("_ZN2nn3hid17SetPalmaBoostModeEb", "Poké Ball Plus boost mode; the host has no Poké Ball Plus", void, h_hid_PalmaBoost, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn3hid24EnableAnyPalmaConnectionEv", "Poké Ball Plus pairing; the host has none", Result, h_hid_PalmaAnyOn, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid25DisableAnyPalmaConnectionEv", "Poké Ball Plus pairing; the host has none", Result, h_hid_PalmaAnyOff, void) { return RES_OK; }
HLE_FORCE("_ZN2nn3hid24GetPalmaConnectionHandleEPNS0_21PalmaConnectionHandleERKj", "Poké Ball Plus connection handle: none connected", Result, h_hid_PalmaHandle, uint64_t *h, const uint32_t *id) {
    (void)id; if (h) *h = 0; return RES_PALMA_INVALID_HANDLE;
}
#define PALMA_FAIL(sym, what, fn, ...) HLE_FORCE(sym, "Poké Ball Plus " what ": fails, none connected", Result, fn, __VA_ARGS__) { return RES_PALMA_INVALID_HANDLE; }
PALMA_FAIL("_ZN2nn3hid15InitializePalmaERKNS0_21PalmaConnectionHandleE", "initialization", h_hid_PalmaInit, const void *h)
PALMA_FAIL("_ZN2nn3hid31BindPalmaOperationCompleteEventERKNS0_21PalmaConnectionHandleEPNS_2os15SystemEventTypeENS4_14EventClearModeE", "operation complete event", h_hid_PalmaBind, const void *h, void *ev, int mode)
PALMA_FAIL("_ZN2nn3hid21GetPalmaOperationInfoEPNS0_18PalmaOperationInfoERKNS0_21PalmaConnectionHandleE", "operation result", h_hid_PalmaOpInfo, void *info, const void *h)
PALMA_FAIL("_ZN2nn3hid17PlayPalmaActivityERKNS0_21PalmaConnectionHandleERKt", "activity", h_hid_PalmaPlay, const void *h, const uint16_t *i)
PALMA_FAIL("_ZN2nn3hid18SetPalmaFrModeTypeERKNS0_21PalmaConnectionHandleERKNS0_15PalmaFrModeTypeE", "FR mode", h_hid_PalmaFr, const void *h, const void *t)
PALMA_FAIL("_ZN2nn3hid13ReadPalmaStepERKNS0_21PalmaConnectionHandleE", "step count", h_hid_PalmaStepRead, const void *h)
PALMA_FAIL("_ZN2nn3hid15EnablePalmaStepERKNS0_21PalmaConnectionHandleEb", "step counting", h_hid_PalmaStepOn, const void *h, unsigned on)
PALMA_FAIL("_ZN2nn3hid14ResetPalmaStepERKNS0_21PalmaConnectionHandleE", "step count reset", h_hid_PalmaStepReset, const void *h)
PALMA_FAIL("_ZN2nn3hid27ReadPalmaApplicationSectionERKNS0_21PalmaConnectionHandleEim", "application section read", h_hid_PalmaAppRead, const void *h, int off, uint64_t n)
PALMA_FAIL("_ZN2nn3hid28WritePalmaApplicationSectionERKNS0_21PalmaConnectionHandleEimRKNS0_35PalmaApplicationSectionAccessBufferE", "application section write", h_hid_PalmaAppWrite, const void *h, int off, uint64_t n, const void *buf)
PALMA_FAIL("_ZN2nn3hid19ReadPalmaUniqueCodeERKNS0_21PalmaConnectionHandleE", "unique code", h_hid_PalmaCode, const void *h)
PALMA_FAIL("_ZN2nn3hid25SetPalmaUniqueCodeInvalidERKNS0_21PalmaConnectionHandleE", "unique code reset", h_hid_PalmaCodeOff, const void *h)
PALMA_FAIL("_ZN2nn3hid23WritePalmaActivityEntryERKNS0_21PalmaConnectionHandleERKtPKNS0_18PalmaActivityEntryE", "activity entry write", h_hid_PalmaActWrite, const void *h, const uint16_t *i, const void *e)
PALMA_FAIL("_ZN2nn3hid28WritePalmaRgbLedPatternEntryERKNS0_21PalmaConnectionHandleERKtPKvm", "LED pattern write", h_hid_PalmaLed, const void *h, const uint16_t *i, const void *p, uint64_t n)
PALMA_FAIL("_ZN2nn3hid19WritePalmaWaveEntryERKNS0_21PalmaConnectionHandleERKNS0_12PalmaWaveSetERKtPKvmm", "sound write", h_hid_PalmaWave, const void *h, const void *set, const uint16_t *i, const void *p, uint64_t n, uint64_t m)
PALMA_FAIL("_ZN2nn3hid37GetPalmaDataBaseIdentificationVersionERKNS0_21PalmaConnectionHandleE", "database version", h_hid_PalmaDbGet, const void *h)
PALMA_FAIL("_ZN2nn3hid37SetPalmaDataBaseIdentificationVersionERKNS0_21PalmaConnectionHandleEi", "database version", h_hid_PalmaDbSet, const void *h, int v)
PALMA_FAIL("_ZN2nn3hid19SuspendPalmaFeatureERKNS0_21PalmaConnectionHandleERKNS_4util10BitFlagSetILi32ENS0_12PalmaFeatureEEE", "feature suspension", h_hid_PalmaSuspend, const void *h, const void *f)
PALMA_FAIL("_ZN2nn3hid16ReadPalmaPlayLogERKNS0_21PalmaConnectionHandleERKt", "play log read", h_hid_PalmaLogRead, const void *h, const uint16_t *i)
PALMA_FAIL("_ZN2nn3hid17ResetPalmaPlayLogERKNS0_21PalmaConnectionHandleERKt", "play log reset", h_hid_PalmaLogReset, const void *h, const uint16_t *i)
