// Players and controllers (GitHub issue #74): the Mac front end's slot assignment (pad_slots.h) and the npads hle_hid.c reports for each player, docked and handheld, with the
// game's supported npad ids; no controller, window or guest exists in this test (the hid take-overs are called directly, events are recorded by the stubs)
#include <unistd.h>
// Scripted input steps (DM_CTL, DM_KEYS) run on threads that sleep: the test owns their sleep, so each one waits at a gate until the test lets it on (no race decides a result)
int test_usleep(useconds_t us);
#define usleep test_usleep
#include "../hle_hid.c"
#undef usleep
#include "../pad_slots.h"

static int checks, fails, g_mode = 1, g_signals;
static void *g_signalled;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
int dm_opmode(void) { return g_mode; }
uint64_t guest_ns(void) { return 1000000000ull; }
uint64_t now_ns(void) { return 1000000000ull; }
uint64_t dm_guest_frame(void) { return 0; }
void dm_touch_set(const dm_touch *t, int n) { (void)t; (void)n; }
int touch_fill_n(void *out, int max, uint64_t now) { (void)out; (void)max; (void)now; return 0; }
int gesture_fill(void *out, int n, uint64_t now) { (void)out; (void)n; (void)now; return 0; }
void dm_gfx_shot_at(long frame, const char *path) { (void)frame; (void)path; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }
void hle_signal_system_event(void *g) { g_signalled = g; g_signals++; }
int hle_signal_bound_system_event(void *g) { g_signalled = g; g_signals++; return 1; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_ctl_peek(uint64_t a, int n) { (void)a; (void)n; }
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) { (void)a; (void)n; (void)path; }
void dm_ctl_cvar_i32(uint64_t off, int32_t v) { (void)off; (void)v; }
void dm_ctl_cvar_f32(uint64_t off, float v) { (void)off; (void)v; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
// DM_CTL's "fps" lines (aoc_timing.c, not built here): the test sends none
int aoc_timing_valid(int target) { (void)target; return 0; }
void aoc_timing_set(int target, int dynamic) { (void)target; (void)dynamic; }
int aoc_timing_target(void) { return 0; }
int aoc_timing_dynamic(void) { return 0; }

static uint64_t buttons_of(uint32_t id, uint32_t style) { NpadState s; fill(&s, id, style); return s.attributes ? s.buttons & 0xffff : 0; }
static int32_t lx_of(uint32_t id) { NpadState s; fill(&s, id, STYLE_FULLKEY); return s.lx; }
static void ids(const uint32_t *v, uint64_t n) { h_hid_SetIds(v, n); }

static void t_slots(void) {
    pad_slots s; memset(&s, 0, sizeof s);
    static char a, b, c, d, b2, kbd;   // controllers stand in as distinct addresses
    CHECK(pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "the first controller is player 1");
    CHECK(pad_slot_take(&s, &b, "DualSense Wireless Controller") == 1, "the second controller is player 2");
    CHECK(pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "taking again keeps the slot");
    CHECK(pad_slot_mask(&s) == 0x3, "two slots held");
    CHECK(pad_slot_free(&s, &b) == 1 && pad_slot_of(&s, &b) < 0, "a disconnected controller frees its slot");
    CHECK(pad_slot_take(&s, &c, "Pro Controller") == 2, "another controller does not take the slot kept for the one that went away");
    CHECK(pad_slot_take(&s, &b2, "DualSense Wireless Controller") == 1, "the same controller reconnected gets its player back");
    CHECK(pad_slot_free(&s, &a) == 0, "player 1's controller goes away");
    CHECK(pad_slot_take(&s, &d, "8BitDo") == 0, "a new controller fills player 1 first");
    CHECK(pad_slot_free(&s, &d) == 0 && pad_slot_take(&s, &a, "Xbox Wireless Controller") == 0, "player 1's slot goes back to its name before any other");
    static char many[DM_PLAYERS];
    for (int i = 0; i < DM_PLAYERS - 3; i++) CHECK(pad_slot_take(&s, &many[i], "Pro Controller") == 3 + i, "the lowest unused slot");
    CHECK(pad_slot_mask(&s) == (1u << DM_PLAYERS) - 1, "every slot held");
    CHECK(pad_slot_take(&s, &many[DM_PLAYERS - 3], "Pro Controller") == -1, "a ninth controller gets no player");
    CHECK(pad_slot_take(&s, NULL, "x") == -1 && pad_slot_free(&s, NULL) == -1, "no owner, no slot");
    memset(&s, 0, sizeof s);   // DM_KBD_PLAYER=own: the keyboard holds player 1 for good
    CHECK(pad_slot_take(&s, &kbd, "keyboard") == 0 && pad_slot_take(&s, &a, "Xbox Wireless Controller") == 1, "with the keyboard on its own the first controller is player 2");
}

static void t_npads(void) {
    // One player (the keyboard, or one controller): as before, everything is player 1 on No1 docked
    g_mode = 1; dm_pad_players(0);
    CHECK(h_hid_GetStyle(&(uint32_t){0}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){1}) == 0 && h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == 0, "one player: No1 only");
    CHECK(dm_player_npad(0) == 0 && dm_player_npad(1) == 1 && dm_player_npad(7) == 7, "player n on No n docked");
    dm_input_set(DM_BTN_X, 100, 0, 0, 0); dm_pad_set(DM_BTN_A, -200, 0, 0, 0);
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_X | DM_BTN_A) && lx_of(0) == -200, "keyboard and player 1's controller are ORed, the stick deflected more wins");
    dm_input_set(0, 0, 0, 0, 0); dm_pad_set(0, 0, 0, 0, 0); buttons_of(0, STYLE_FULLKEY);

    // Two controllers: player 2 on No2 with its own input, the style event of No2 signalled
    static uint8_t ev1[0x40];
    h_hid_BindStyleEv(&(uint32_t){1}, ev1, 0);
    CHECK(g_signals == 0, "no signal on binding an npad with no controller");
    dm_pad_players(0x3);
    CHECK(g_signals == 1 && g_signalled == ev1, "No2's style event signalled when player 2's controller connects");
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "player 2 is a full-key controller on No2");
    dm_pad_set_player(1, DM_BTN_B, 300, 0, 0, 0); dm_pad_set(DM_BTN_Y, 0, 0, 0, 0);
    CHECK(buttons_of(1, STYLE_FULLKEY) == DM_BTN_B && lx_of(1) == 300, "player 2 reads its own controller");
    CHECK(buttons_of(0, STYLE_FULLKEY) == DM_BTN_Y && lx_of(0) == 0, "player 1 does not see player 2");
    CHECK(buttons_of(1, STYLE_HANDHELD) == 0, "player 2 is not a handheld controller");
    CHECK(h_hid_Led(&(uint32_t){0}) == 0x1 && h_hid_Led(&(uint32_t){1}) == 0x3 && h_hid_Led(&(uint32_t){7}) == 0x6, "player LED patterns");
    int32_t info[2]; int8_t arg[8] = {1, 4};
    CHECK(h_hid_ShowCtrl(info, arg) == RES_OK && info[0] == 2 && info[1] == 0, "the controller applet reports two players, player 1 selected");
    arg[1] = 1; h_hid_ShowCtrl(info, arg);
    CHECK(info[0] == 1, "the applet's count is capped at the game's maximum");
    CHECK(six_connected(0x0100 | 0) && !six_connected(0x0200 | 0), "player 2 has a six-axis sensor, No3 none");

    // A one-player game ({No1, Handheld}): player 2's controller plays as player 1, No2 reads as not connected
    g_signals = 0; ids((const uint32_t[]){0, NPAD_HANDHELD}, 2);
    CHECK(g_signals == 1 && g_signalled == ev1, "No2's style event signalled when the game stops supporting it");
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && buttons_of(1, STYLE_FULLKEY) == 0, "No2 not reported");
    CHECK(buttons_of(0, STYLE_FULLKEY) == (DM_BTN_Y | DM_BTN_B) && lx_of(0) == 300, "player 2's controller feeds player 1");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 1, "the applet reports one player");

    // A four-player game: a fifth controller is not used, nor merged into player 1
    ids((const uint32_t[]){0, 1, 2, 3, NPAD_HANDHELD}, 5);
    dm_pad_players(0x1f); dm_pad_set_player(4, DM_BTN_ZR, 0, 0, 0, 0);
    CHECK(h_hid_GetStyle(&(uint32_t){3}) == STYLE_FULLKEY && h_hid_GetStyle(&(uint32_t){4}) == 0, "players 1..4 reported, player 5 not");
    CHECK(!(buttons_of(0, STYLE_FULLKEY) & DM_BTN_ZR), "player 5 does not feed player 1 in a game for four");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 4, "the applet reports four players");

    // Player 2 goes away: No2 disconnected, its input released
    dm_pad_players(0x1);
    CHECK(h_hid_GetStyle(&(uint32_t){1}) == 0 && atomic_load(&g_pad_btn[1]) == 0, "a disconnected player's npad and input are cleared");

    // Handheld mode: player 1 on Handheld, player 2 on No2 (No1 stays empty)
    g_mode = 0; dm_pad_players(0x3); dm_pad_set_player(1, DM_BTN_L, 0, 0, 0, 0);
    CHECK(dm_player_npad(0) == NPAD_HANDHELD && dm_player_npad(1) == 1, "handheld: player 1 on Handheld, player 2 on No2");
    CHECK(h_hid_GetStyle(&(uint32_t){NPAD_HANDHELD}) == STYLE_HANDHELD && h_hid_GetStyle(&(uint32_t){0}) == 0 && h_hid_GetStyle(&(uint32_t){1}) == STYLE_FULLKEY, "handheld styles");
    CHECK(buttons_of(NPAD_HANDHELD, STYLE_HANDHELD) == DM_BTN_Y && buttons_of(1, STYLE_FULLKEY) == DM_BTN_L, "handheld: each player its own input");
    h_hid_ShowCtrl(info, NULL);
    CHECK(info[0] == 2 && info[1] == NPAD_HANDHELD, "handheld: the applet selects the handheld controller");
}

static dm_haptic_state vibration[DM_PLAYERS];
static int vibration_calls;
static void vibration_sink(dm_haptic_state s) { vibration[s.player] = s; vibration_calls++; }
static void t_vibration(void) {
    uint32_t h[2] = {0}, id = 0, info[2];
    dm_vibration v = {0.25, 160, 0.5, 320}, got;
    g_mode = 1; ids((uint32_t[]){0, 1, NPAD_HANDHELD}, 3); dm_pad_players(3);
    CHECK(!h_hid_VibPermit() && !h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY), "no sink: no vibration devices");
    dm_haptics_init(vibration_sink, 1); dm_haptics_route(0, 1); dm_haptics_route(1, 1);
    CHECK(h_hid_VibPermit() && h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY) == 2, "two vibration motors available");
    CHECK(h[0] == 3 && h[1] == 0x10003, "SDK vibration handle bytes: style 3, npad 0, motors 0 and 1");
    CHECK(!h_hid_VibHandles(h, 2, &id, STYLE_HANDHELD), "wrong controller style gets no motors");
    CHECK(!h_hid_VibHandles(h, 2, &(uint32_t){2}, STYLE_FULLKEY), "disconnected player gets no motors");
    CHECK(h_hid_VibHandles(h, 1, &id, STYLE_FULLKEY) == 1, "handle count respects caller capacity");
    h_hid_SendVib(&h[0], &v); h_hid_VibActual(&got, &h[0]);
    CHECK(!memcmp(&got, &v, sizeof v) && vibration[1].motor[0].amplitude_low == 0, "game value is retained for the addressed player only");
    h_hid_VibInfo(info, &h[1]); CHECK(info[0] == 1 && info[1] == 2, "right motor is a right LRA");
    uint32_t invalid[] = {0, 0x20003, 0x1000003, 0x203};
    vibration_calls = 0;
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) h_hid_SendVib(&invalid[i], &v);
    CHECK(!vibration_calls, "invalid style, motor, reserved byte and disconnected player are ignored");
    dm_vibration pair[] = {{0.1, 160, 0, 320}, {0.6, 160, 0, 320}};
    h_hid_SendVibs(h, pair, 2);
    CHECK(vibration_calls == 1 && vibration[0].motor[0].amplitude_low == 0.1f && vibration[0].motor[1].amplitude_low == 0.6f, "HID batch publishes one complete pair");
    h_hid_InitVib(&h[0]);
    CHECK(dm_haptics_actual(0, 0).amplitude_low == 0 && dm_haptics_actual(0, 1).amplitude_low == 0.6f, "initializing one motor leaves its partner intact");
    ids((uint32_t[]){0, NPAD_HANDHELD}, 2);
    h_hid_SendVib(&h[0], &v);
    CHECK(dm_haptics_actual(1, 0).amplitude_low == 0.25f, "one-player rumble reaches controllers whose input is merged into player 1");
    dm_haptics_route(0, 0);
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_FULLKEY) == 2, "merged controller supplies rumble when player 1 has no physical output");
    g_mode = 0; id = NPAD_HANDHELD;
    CHECK(h_hid_VibHandles(h, 2, &id, STYLE_HANDHELD) == 2 && h[0] == 0x2004 && h[1] == 0x12004, "handheld SDK vibration handles");
    CHECK(!vib_targets(3), "old docked handle is not valid in handheld mode");
    dm_haptics_route(1, 0);
    CHECK(!h_hid_VibPermit(), "disconnecting the last output disables vibration");
}

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_calls, g_gate_allowed, g_gate_open, g_gate_stop, g_ended;   // sleeps reached, let through; open: every sleep returns at once; stop: a sleeping script ends
static pthread_key_t g_script_key;   // set on a script's thread at its first gated sleep; its destructor counts the thread's end
static void script_ended(void *v) { (void)v; pthread_mutex_lock(&g_gate_mu); g_ended++; pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu); }
int test_usleep(useconds_t us) {
    (void)us; if (g_gate_open) return 0;
    if (!pthread_getspecific(g_script_key)) pthread_setspecific(g_script_key, &g_script_key);
    pthread_mutex_lock(&g_gate_mu); int me = ++g_gate_calls; pthread_cond_broadcast(&g_gate_cv);
    while (g_gate_allowed < me && !g_gate_stop) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    int stop = g_gate_stop; pthread_mutex_unlock(&g_gate_mu);
    if (stop) pthread_exit(NULL);   // DM_CTL's thread polls its file for good: the test ends it here, between polls (its file is closed)
    return 0;
}
static int scripts_ended(int n) {   // 1 once n script threads have ended, 0 after 5 s
    struct timespec end; clock_gettime(CLOCK_REALTIME, &end); end.tv_sec += 5; int ok = 1;
    pthread_mutex_lock(&g_gate_mu);
    while (g_ended < n && ok) ok = !pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &end);
    int done = g_ended >= n; pthread_mutex_unlock(&g_gate_mu); return done;
}
static void gate_reached(int n) {   // a script that never gets there fails the test after 5 s instead of hanging it
    struct timespec end; clock_gettime(CLOCK_REALTIME, &end); end.tv_sec += 5;
    pthread_mutex_lock(&g_gate_mu);
    while (g_gate_calls < n) if (pthread_cond_timedwait(&g_gate_cv, &g_gate_mu, &end)) { fprintf(stderr, "FAIL: the script never reached sleep %d\n", n); exit(1); }
    pthread_mutex_unlock(&g_gate_mu);
}
static void gate_let(int n) { pthread_mutex_lock(&g_gate_mu); g_gate_allowed = n; pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu); }
static uint64_t game_read(void) { NpadState s; h_hid_StateFullKey(&s, &(uint32_t){0}); return s.buttons & 0xffff; }   // a frame's read, as the game makes it
// Steps 1 and 2 of a script that presses A twice, from the first gate on: the game reads at the chosen moments only
static void two_presses(const char *path, int first, int read_while_held) {
    gate_reached(first); CHECK(atomic_load(&g_pad_btn[0]) == DM_BTN_A, path);
    uint64_t r = atomic_load(&g_released);
    if (read_while_held) CHECK(game_read() == DM_BTN_A && atomic_load(&g_released) == r, "a sample taken while held never counts as the release");
    gate_let(first); gate_reached(first + 1);
    CHECK(atomic_load(&g_pad_btn[0]) == 0, "let go, and the next press waits for the game to see it");
    int g = first + 1;
    if (!read_while_held) {   // the press was latched and no frame read it: the first read after the release still gets A, and does not count
        CHECK(game_read() == DM_BTN_A && atomic_load(&g_released) == r, "a pending press that the first read consumes does not count as the release");
        gate_let(g); gate_reached(++g); CHECK(atomic_load(&g_pad_btn[0]) == 0, "still waiting for a released sample");
    }
    CHECK(game_read() == 0 && atomic_load(&g_released) == r + 1, "the game gets a released sample");
    gate_let(g); gate_reached(g + 1);
    CHECK(atomic_load(&g_pad_btn[0]) == DM_BTN_A && game_read() == DM_BTN_A, "then the second press: two presses with a release between them");
    gate_let(g + 1); gate_reached(g + 2); CHECK(game_read() == 0, "the second press is let go and seen");
    gate_let(g + 2);
}
static void t_script(void) {
    pthread_key_create(&g_script_key, script_ended);
    g_mode = 1; dm_pad_players(0); ids((uint32_t[]){0, NPAD_HANDHELD}, 2); dm_input_set(0, 0, 0, 0, 0); dm_pad_set(0, 0, 0, 0, 0); game_read();
    g_gate_open = 1;   // nobody reads: the wait ends after its 1 s and says the release was not seen
    key_step st = {0, 0.15, DM_BTN_A};
    CHECK(!script_press(&st), "no released sample within the time limit: reported, not assumed");
    g_gate_open = 0; game_read();
    unsetenv("DM_CTL"); setenv("DM_KEYS", "0:A,0:A", 1); hid_keys_init();
    two_presses("DM_KEYS presses A", 1, 0);
    CHECK(scripts_ended(1), "DM_KEYS's thread ends after its last step");
    char path[] = "/tmp/t_hid_ctl_XXXXXX"; int fd = mkstemp(path);
    CHECK(fd >= 0 && write(fd, "A\nA\n", 4) == 4, "a DM_CTL file"); close(fd);
    int base = g_gate_calls; unsetenv("DM_KEYS"); setenv("DM_CTL", path, 1); ctl_start();
    two_presses("DM_CTL presses A", base + 1, 1);
    gate_reached(base + 5);   // its file read to the end, back to polling it
    pthread_mutex_lock(&g_gate_mu); g_gate_stop = 1; pthread_cond_broadcast(&g_gate_cv); pthread_mutex_unlock(&g_gate_mu);
    CHECK(scripts_ended(2), "every script thread the test started has ended");
    unlink(path);
}

int main(void) {
    t_slots();
    t_npads();
    t_vibration();
    t_script();   // last: it sets its own supported npads
    printf("hid players: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
