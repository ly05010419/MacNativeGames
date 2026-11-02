/* Live map survey for the selected TOTK version. No arbitrary memory writes and no raw position pokes.
   Offset leads: aquacluck/totk-lotuskit 59dcfcc; independently checked against our main in
   analysis/continuous-survey-entry-named.c. forceSetMatrix routes through the physics component,
   or queues a next transform, unlike merely changing the actor's displayed position.
   Only called after the actual game frame-end GPU wait, never from the input file thread. */
#include "world_survey.h"
#include "game_version.h"
#include <ctype.h>
#include <limits.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef GAME_WONDER
/* Super Mario Bros. Wonder: the survey's offsets are TOTK's; none of it runs. */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only once the survey found the system object, which it never does here */
void runtime_survey_init(Module *m) {
    (void)m;
    if (getenv("TOTK_SURVEY")) runtime_fail("survey: not available for Super Mario Bros. Wonder");
}
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) { }
#else
#ifdef GAME_BOTW
/* Breath of the Wild 1.6.0: no frame-end hook (the survey sync object is never found, see runtime_survey_sync), so
   "survey pos" reads and "survey move" writes the player's position from the command thread. The pointer chains from
   main's data segment are the ones Bloom (Swiffy22) found for 1.6.0 (the botw-save-state tool uses them); the code is
   our own. Chains: Havok position = [[[main+0x2d37af0]+0xf0]+0xd28]+0x5e0 (float xyz);
                    world matrix   = [[main+0x2ca1140]+0x60]+0x398 (3x4 row-major, translation at [3], [7], [11]);
                    camera         = [[main+0x2ce1090]+0x60]+0x20 {position, look-at, up, fovy, ?, aspect}.
   Writing those two is not enough: the actor and the Havok object keep a dozen more copies of the position (feet,
   previous positions, swept transforms), so a move shifts every copy near the player by the same offset ("survey scan"
   lists them). A far place is not loaded, and the player freezes there; the move is therefore a flight at a speed the
   streaming keeps up with (2 m per frame worked, 8 m did not): survey move <tag> X Y Z <frames>, 1200 frames for 2.3 km.
   Verified on the Mac 2026-10-21 (5 m step: walks, camera follows; Hebra to Oman Au shrine: lands, in control). */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only once the survey found the system object, which it never does here */
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) { }
#if defined(GAME_BOTW160)
enum { HAVOK_ROOT = 0x2d37af0, HAVOK_STEP1 = 0xf0, HAVOK_STEP2 = 0xd28, HAVOK_POS = 0x5e0,
       MATRIX_ROOT = 0x2ca1140, MATRIX_STEP1 = 0x60, MATRIX_OFF = 0x398,
       CAMERA_ROOT = 0x2ce1090, CAMERA_STEP1 = 0x60, CAMERA_OFF = 0x20 };   /* camera pan matrix (same research) */
static uintptr_t survey_main;
static _Atomic int survey_enabled;
static int read_mem(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t read_ptr(uintptr_t address) { uintptr_t p = 0; return read_mem(address, &p, sizeof p) ? p : 0; }
static int valid_xyz(const float xyz[3]) {
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) &&
           fabsf(xyz[0]) <= 6000 && xyz[1] >= -1000 && xyz[1] <= 3000 && fabsf(xyz[2]) <= 6000;
}
/* The two positions, or 0 when a link of a chain is missing (before the world is up) or the values are not a place. */
static uintptr_t havok_position(void) {
    uintptr_t a = read_ptr(survey_main + HAVOK_ROOT); if (a) a = read_ptr(a + HAVOK_STEP1); if (a) a = read_ptr(a + HAVOK_STEP2);
    float xyz[3];
    return a && read_mem(a + HAVOK_POS, xyz, sizeof xyz) && valid_xyz(xyz) ? a + HAVOK_POS : 0;
}
static uintptr_t world_matrix(void) {
    uintptr_t a = read_ptr(survey_main + MATRIX_ROOT); if (a) a = read_ptr(a + MATRIX_STEP1);
    float m[12];
    if (!a || !read_mem(a + MATRIX_OFF, m, sizeof m)) return 0;
    for (int i = 0; i < 12; i++) if (!isfinite(m[i])) return 0;
    float t[3] = {m[3], m[7], m[11]};
    return valid_xyz(t) ? a + MATRIX_OFF : 0;
}
void runtime_survey_init(Module *m) {
    const char *enabled = getenv("TOTK_SURVEY");
    if (!enabled || strcmp(enabled, "1")) return;
    const char *owner = getenv("TOTK_SLOT_OWNER"), *profile = getenv("TOTK_PROFILE_DIR");
    char target[PATH_MAX];
    if (!owner || !strcmp(owner, "user") || !profile || !realpath(profile, target))
        runtime_fail("TOTK_SURVEY requires an agent-owned session and a separate test profile");
    if (strcmp(m->name, "main") || HAVOK_ROOT + 8u > m->size || MATRIX_ROOT + 8u > m->size)
        runtime_fail("survey: Breath of the Wild 1.6.0 main expected");
    survey_main = (uintptr_t)m->base;
    atomic_store(&survey_enabled, 1);
    fprintf(stderr, "[survey] enabled (Breath of the Wild 1.6.0): pos and move act on the player's Havok position and world matrix\n");
}
static void report(const char *tag, const char *status) {
    uintptr_t h = havok_position(), w = world_matrix();
    float xyz[3] = {0}, m[12] = {0};
    if (h) read_mem(h, xyz, sizeof xyz);
    if (w) read_mem(w, m, sizeof m);
    fprintf(stderr, "[survey] tag=%s status=%s havok=%#llx xyz=%.6f,%.6f,%.6f matrix=%#llx xyz=%.6f,%.6f,%.6f\n",
            tag, status, (unsigned long long)h, xyz[0], xyz[1], xyz[2], (unsigned long long)w, m[3], m[7], m[11]);
}
/* survey scan X Y Z: diagnostic, lists every float triple within 2 m of X Y Z in the player actor (first 0x4000 bytes of
   [[main+0x2ca1140]+0x60]), the Havok object ([[main+0x2d37af0]+0xf0]+0xd28, 0x1000 bytes) and the actor's +0xf0 object. */
static void scan_block(const char *what, uintptr_t base, size_t size, const float xyz[3]) {
    float *buf = malloc(size); if (!buf) return;
    if (read_mem(base, buf, size)) {
        for (size_t i = 0; i + 3 <= size / 4; i++)
            if (fabsf(buf[i] - xyz[0]) < 2 && fabsf(buf[i + 1] - xyz[1]) < 2 && fabsf(buf[i + 2] - xyz[2]) < 2)
                fprintf(stderr, "[survey-scan] %s+%#zx = %.3f,%.3f,%.3f\n", what, i * 4, buf[i], buf[i + 1], buf[i + 2]);
    } else fprintf(stderr, "[survey-scan] %s at %#llx unreadable\n", what, (unsigned long long)base);
    free(buf);
}
static void survey_scan(const float xyz[3]) {
    uintptr_t actor = read_ptr(survey_main + MATRIX_ROOT); if (actor) actor = read_ptr(actor + MATRIX_STEP1);
    uintptr_t h = read_ptr(survey_main + HAVOK_ROOT); if (h) h = read_ptr(h + HAVOK_STEP1); if (h) h = read_ptr(h + HAVOK_STEP2);
    fprintf(stderr, "[survey-scan] actor=%#llx havok=%#llx looking for %.2f,%.2f,%.2f\n", (unsigned long long)actor, (unsigned long long)h, xyz[0], xyz[1], xyz[2]);
    if (actor) scan_block("actor", actor, 0x4000, xyz);
    if (h) scan_block("havok", h, 0x1000, xyz);
    uintptr_t h1 = read_ptr(survey_main + HAVOK_ROOT); if (h1) h1 = read_ptr(h1 + HAVOK_STEP1);
    if (h1) scan_block("havok1", h1, 0x1000, xyz);
}
int runtime_survey_command(const char *line) {
    if (strncmp(line, "survey ", 7)) return 0;
    char op[16], tag[64]; float xyz[3] = {0}; unsigned hold = 0; int used = 0, kind = 0;
    if (sscanf(line, "survey %15s %63s %n", op, tag, &used) == 2 && used && !line[used] && !strcmp(op, "pos")) kind = 1;
    used = 0;
    if (sscanf(line, "survey move %63s %f %f %f %u %n", tag, &xyz[0], &xyz[1], &xyz[2], &hold, &used) == 5 &&
        used && !line[used] && valid_xyz(xyz) && hold >= 1 && hold <= 3000) kind = 2;
    used = 0;
    if (sscanf(line, "survey scan %f %f %f %n", &xyz[0], &xyz[1], &xyz[2], &used) == 3 && used && !line[used]) {
        if (atomic_load(&survey_enabled)) survey_scan(xyz); return 1;
    }
    if (!kind) { fprintf(stderr, "[survey] rejected: expected pos <tag> or move <tag> X Y Z <1..3000 frames>\n"); return 1; }
    for (const char *p = tag; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
        fprintf(stderr, "[survey] rejected: invalid tag\n"); return 1;
    }
    if (!atomic_load(&survey_enabled)) { fprintf(stderr, "[survey] tag=%s status=disabled\n", tag); return 1; }
    if (kind == 1) { report(tag, havok_position() ? "position" : "no_player"); return 1; }
    uintptr_t h = havok_position(), w = world_matrix();
    if (!h || !w) { report(tag, "no_player"); return 1; }
    report(tag, "move_begin");
    /* Every float triple within 2 m of the player's position in the actor, the Havok object and its parent, plus the
       world matrix and the camera matrix translations, keeps its offset from the player and moves with him: one pass
       collects them, then they are written for <hold> frames (30 Hz) so the physics settles on the new place. */
    float cur[3]; read_mem(h, cur, sizeof cur);
    struct { uintptr_t at; float rel[3]; } slot[160]; unsigned n = 0;
    uintptr_t actor = read_ptr(survey_main + MATRIX_ROOT); if (actor) actor = read_ptr(actor + MATRIX_STEP1);
    uintptr_t hk1 = read_ptr(survey_main + HAVOK_ROOT); if (hk1) hk1 = read_ptr(hk1 + HAVOK_STEP1);
    uintptr_t hk = hk1 ? read_ptr(hk1 + HAVOK_STEP2) : 0;
    uintptr_t cam0 = read_ptr(survey_main + CAMERA_ROOT), cam = cam0 ? read_ptr(cam0 + CAMERA_STEP1) : 0;
    fprintf(stderr, "[survey] actor=%#llx havok=%#llx havok1=%#llx cam0=%#llx cam=%#llx\n", (unsigned long long)actor, (unsigned long long)hk, (unsigned long long)hk1, (unsigned long long)cam0, (unsigned long long)cam);
    /* the camera block at CAMERA_OFF is {position, look-at, up, fovy, ?, aspect}: position and look-at sit within 10 m of the player */
    struct { uintptr_t base; size_t size; float window; } blocks[4] = {{actor, 0x4000, 2}, {hk, 0x1000, 2}, {hk1, 0x1000, 2}, {cam ? cam + CAMERA_OFF : 0, 0x18, 12}};
    for (unsigned b = 0; b < 4; b++) {
        if (!blocks[b].base) continue;
        float *buf = malloc(blocks[b].size); if (!buf) continue;
        float win = blocks[b].window;
        if (read_mem(blocks[b].base, buf, blocks[b].size))
            for (size_t k = 0; k + 3 <= blocks[b].size / 4 && n < 150; k++)
                if (fabsf(buf[k] - cur[0]) < win && fabsf(buf[k + 1] - cur[1]) < win && fabsf(buf[k + 2] - cur[2]) < win) {
                    slot[n].at = blocks[b].base + k * 4;
                    for (int c = 0; c < 3; c++) slot[n].rel[c] = buf[k + c] - cur[c];
                    n++; k += 2;
                }
        free(buf);
    }
    /* matrix translations are not contiguous: world matrix [3],[7],[11]; camera matrix at CAMERA_OFF the same */
    uintptr_t mats[1] = {w}; unsigned nmat = 0; struct { uintptr_t at; float rel[3]; } mat[1];
    for (unsigned q = 0; q < 1; q++) {
        float m[12];
        if (!mats[q] || !read_mem(mats[q], m, sizeof m)) continue;
        mat[nmat].at = mats[q]; mat[nmat].rel[0] = m[3] - cur[0]; mat[nmat].rel[1] = m[7] - cur[1]; mat[nmat].rel[2] = m[11] - cur[2];
        fprintf(stderr, "[survey] %s matrix at %#llx: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", q ? "camera" : "world", (unsigned long long)mats[q],
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
        if (q == 1 && (fabsf(mat[nmat].rel[0]) > 300 || fabsf(mat[nmat].rel[1]) > 300 || fabsf(mat[nmat].rel[2]) > 300)) continue;   /* camera not near the player: leave it */
        nmat++;
    }
    fprintf(stderr, "[survey] tag=%s moving %u triples and %u matrices by %.2f,%.2f,%.2f\n", tag, n, nmat, xyz[0] - cur[0], xyz[1] - cur[1], xyz[2] - cur[2]);
    /* A far place is not loaded: the player is frozen until the world streams in around him, which never happens when he
       appears there at once. So the move is a flight: a straight line from here to there over <hold> frames (30 Hz), the
       world streaming in on the way as it does for a fast glide. 300 frames carry 2.3 km at about 8 m per frame. */
    for (unsigned i = 0; i < hold; i++) {
        float f = (float)(i + 1) / (float)hold, at[3];
        for (int c = 0; c < 3; c++) at[c] = cur[c] + (xyz[c] - cur[c]) * f;
        for (unsigned k = 0; k < n; k++) { float v[3] = {at[0] + slot[k].rel[0], at[1] + slot[k].rel[1], at[2] + slot[k].rel[2]}; memcpy((void *)slot[k].at, v, sizeof v); }
        for (unsigned k = 0; k < nmat; k++) { float *m = (float *)mat[k].at; m[3] = at[0] + mat[k].rel[0]; m[7] = at[1] + mat[k].rel[1]; m[11] = at[2] + mat[k].rel[2]; }
        usleep(33333);
    }
    report(tag, "move_done");
    return 1;
}
#endif   /* GAME_BOTW160 */
#if !defined(GAME_BOTW160)
void runtime_survey_init(Module *m) {
    (void)m;
    if (getenv("TOTK_SURVEY")) runtime_fail("survey: only Breath of the Wild 1.6.0 has the player position chains");
}
int runtime_survey_command(const char *line) { (void)line; return 0; }
#endif
#else
enum { SCENE_SLOT = GAME_ADDR(0x4663f08), FORCE_MATRIX = GAME_ADDR(0x69a18c), ACTOR_POS = GAME_ACTOR_POS, ACTOR_ROT = GAME_ACTOR_ROT };
static uintptr_t survey_main;
static void (*survey_force)(void *, const float *, uint32_t);
static _Atomic int survey_enabled;
_Atomic int runtime_survey_pending;
/* The frame-end sync, read by hle_nvn.c (built once for all versions): *(sys+[0]), then +[1] through one more pointer if
   [1] >= 0. 1.4.3's frame controller (main+0x2db94c) first waits on *(*(sys+0x150)+0x78), found with TOTK_SURVEY_DEBUG. */
#if defined(TOTK_GAME_V143)
const int32_t runtime_survey_sync[2] = {0x150, 0x78};
#else
const int32_t runtime_survey_sync[2] = {0x1d8, -1};
#endif
static pthread_mutex_t survey_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int kind; char tag[64]; float xyz[3]; unsigned hold; } request;
static unsigned hold_left;
static float hold_xyz[3];
static char hold_tag[64];
static _Thread_local int in_tick;

static int read_mem(uintptr_t address, void *out, size_t size) {
    if (!address || address > UINTPTR_MAX - size) return 0;
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
static uintptr_t read_ptr(uintptr_t address) { uintptr_t p = 0; return read_mem(address, &p, sizeof p) ? p : 0; }
static int valid_xyz(const float xyz[3]) {
    return isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]) &&
           fabsf(xyz[0]) <= 5500 && xyz[1] >= -1500 && xyz[1] <= 3500 && fabsf(xyz[2]) <= 5000;
}
static void survey_debug_layout(uintptr_t module, uintptr_t scene) {
    static unsigned reports;
    if (!getenv("TOTK_SURVEY_DEBUG") || reports++ >= 3) return;
    fprintf(stderr, "[survey-layout] module=%#llx scene=%#llx\n", (unsigned long long)module, (unsigned long long)scene);
    int count = 0; read_mem(scene + 0x50, &count, 4);
    uintptr_t components = read_ptr(scene + 0x58);
    fprintf(stderr, "[survey-layout] components=%#llx count=%d\n", (unsigned long long)components, count);
    if (count < 1 || count > 256) return;
    for (int i = 0; i < count; ++i) {
        uintptr_t c = read_ptr(components + i * 8), ref = read_ptr(c + 8);
        char name[200] = {0}; read_mem(ref, name, sizeof name - 1);
        int n = 0; read_mem(c + 0x20, &n, 4); uintptr_t links = read_ptr(c + 0x28);
        fprintf(stderr, "[survey-layout] component=%d ptr=%#llx ref=%.199s count=%d links=%#llx\n", i, (unsigned long long)c, name, n, (unsigned long long)links);
        if (i != 13 || n < 1 || n > 64) continue;
        for (int j = 0; j < n; ++j) {
            uintptr_t data = read_ptr(links + j * 0x70 + 8), actor = read_ptr(data + 0x40), actor_name = read_ptr(actor + GAME_ACTOR_NAME);
            char text[64] = {0}; read_mem(actor_name, text, sizeof text - 1);
            fprintf(stderr, "[survey-layout] link=%d data=%#llx actor=%#llx name=%.63s\n", j, (unsigned long long)data, (unsigned long long)actor, text);
        }
    }
}
static uintptr_t player_actor(void) {
    uintptr_t module = read_ptr(survey_main + SCENE_SLOT);
    uintptr_t scene = module ? read_ptr(module + 0x1e8) : 0;
    survey_debug_layout(module, scene);
    int count = 0;
    if (!scene || !read_mem(scene + 0x50, &count, 4) || count < 14 || count > 256) return 0;
    uintptr_t components = read_ptr(scene + 0x58);
    uintptr_t resident = components ? read_ptr(components + 13 * 8) : 0;
    if (!resident || !read_mem(resident + 0x20, &count, 4) || count < 1 || count > 64) return 0;
    uintptr_t links = read_ptr(resident + 0x28);
    if (!links) return 0;
    for (int i = 0; i < count; ++i) {
        uintptr_t data = read_ptr(links + (uintptr_t)i * 0x70 + 8);
        uintptr_t actor = data ? read_ptr(data + 0x40) : 0;
        uintptr_t name = actor ? read_ptr(actor + GAME_ACTOR_NAME) : 0;
        char text[7];
        if (name && read_mem(name, text, sizeof text) && !memcmp(text, "Player", 7)) {
            float xyz[3];
            if (read_mem(actor + ACTOR_POS, xyz, sizeof xyz) && valid_xyz(xyz)) return actor;
        }
    }
    return 0;
}
void runtime_survey_init(Module *m) {
    const char *enabled = getenv("TOTK_SURVEY");
    if (!enabled || strcmp(enabled, "1")) return;
    const char *owner = getenv("TOTK_SLOT_OWNER"), *profile = getenv("TOTK_PROFILE_DIR");
    char target[PATH_MAX], user[PATH_MAX];
    if (!owner || !strcmp(owner, "user") || !profile || !realpath(profile, target) ||
        (realpath("analysis/phase6-profile", user) && !strcmp(target, user)))
        runtime_fail("TOTK_SURVEY requires an agent-owned session and a separate test profile");
#if defined(TOTK_GAME_V143)
    const uint32_t expected[] = {0xa9bc7bfd, 0xa9015ff8, 0xa90257f6, 0xa9034ff4};   // 1.4.3 inlines the physics call: larger frame
#else
    const uint32_t expected[] = {0xa9bd7bfd, 0xf9000bf5, 0x910003fd, 0xa9024ff4};
#endif
    if (strcmp(m->name, "main") || SCENE_SLOT + 8u > m->size || FORCE_MATRIX + sizeof expected > m->text_end ||
        memcmp(m->base + FORCE_MATRIX, expected, sizeof expected))
        runtime_fail("survey requires verified game-version forceSetMatrix entry");
    survey_main = (uintptr_t)m->base;
    survey_force = (void (*)(void *, const float *, uint32_t))(m->base + FORCE_MATRIX);
    survey_enabled = 1;
    fprintf(stderr, "[survey] enabled: test profile, game frame-end commands, original forceSetMatrix\n");
}
static void report(const char *tag, const char *status, uint64_t frame, uintptr_t actor) {
    float xyz[3];
    if (actor && read_mem(actor + ACTOR_POS, xyz, sizeof xyz) && valid_xyz(xyz))
        fprintf(stderr, "[survey] tag=%s status=%s frame=%llu actor=%#llx xyz=%.6f,%.6f,%.6f\n",
                tag, status, (unsigned long long)frame, (unsigned long long)actor, xyz[0], xyz[1], xyz[2]);
    else fprintf(stderr, "[survey] tag=%s status=%s frame=%llu actor=0\n", tag, status, (unsigned long long)frame);
}
int runtime_survey_command(const char *line) {
    if (strncmp(line, "survey ", 7)) return 0;
    char op[16], tag[64]; float xyz[3] = {0}; unsigned hold = 0; int used = 0, kind = 0;
    if (sscanf(line, "survey %15s %63s %n", op, tag, &used) == 2 && used && !line[used] && !strcmp(op, "pos")) kind = 1;
    used = 0;
    if (sscanf(line, "survey move %63s %f %f %f %u %n", tag, &xyz[0], &xyz[1], &xyz[2], &hold, &used) == 5 &&
        used && !line[used] && valid_xyz(xyz) && hold >= 1 && hold <= 600) kind = 2;
    if (!kind) { fprintf(stderr, "[survey] rejected: expected pos <tag> or move <tag> X Y Z <1..600 frames>\n"); return 1; }
    for (const char *p = tag; *p; ++p) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') {
        fprintf(stderr, "[survey] rejected: invalid tag\n"); return 1;
    }
    pthread_mutex_lock(&survey_lock);
    if (!survey_enabled) report(tag, "disabled", 0, 0);
    else if (request.kind || hold_left) report(tag, "busy", 0, 0);
    else {
        request.kind = kind; strcpy(request.tag, tag); memcpy(request.xyz, xyz, sizeof xyz); request.hold = hold;
        atomic_store_explicit(&runtime_survey_pending, 1, memory_order_release);
    }
    pthread_mutex_unlock(&survey_lock);
    return 1;
}
void runtime_survey_cancel_user(void) {
    if (!atomic_load_explicit(&runtime_survey_pending, memory_order_relaxed)) return;
    pthread_mutex_lock(&survey_lock);
    request.kind = 0; hold_left = 0; atomic_store(&runtime_survey_pending, 0);
    fprintf(stderr, "[survey] cancelled: user input\n");
    pthread_mutex_unlock(&survey_lock);
}
void runtime_survey_tick(uint64_t frame) {
    if (in_tick || !atomic_load_explicit(&runtime_survey_pending, memory_order_acquire)) return;
    in_tick = 1;
    pthread_mutex_lock(&survey_lock);
    uintptr_t actor = player_actor();
    if (!actor) {
        report(request.kind ? request.tag : hold_tag, "no_player", frame, 0);
        request.kind = 0; hold_left = 0;
    } else if (request.kind == 1) {
        report(request.tag, "position", frame, actor); request.kind = 0;
    } else {
        if (request.kind == 2) {
            memcpy(hold_xyz, request.xyz, sizeof hold_xyz); strcpy(hold_tag, request.tag);
            hold_left = request.hold; request.kind = 0;
            report(hold_tag, "move_begin", frame, actor);
        }
        if (hold_left) {
            float rot[9] = {0}, matrix[12];
            int ok = read_mem(actor + ACTOR_ROT, rot, sizeof rot);
            for (unsigned i = 0; i < 9; i++) if (!isfinite(rot[i]) || fabsf(rot[i]) > 2) ok = 0;
            if (ok) {
                for (unsigned row = 0; row < 3; row++) {
                    for (unsigned col = 0; col < 3; col++) matrix[row * 4 + col] = rot[row * 3 + col];
                    matrix[row * 4 + 3] = hold_xyz[row];
                }
                survey_force((void *)actor, matrix, 0);
                if (!--hold_left) report(hold_tag, "move_done", frame, actor);
            } else { report(hold_tag, "invalid_rotation", frame, actor); hold_left = 0; }
        }
    }
    atomic_store_explicit(&runtime_survey_pending, request.kind || hold_left, memory_order_release);
    pthread_mutex_unlock(&survey_lock);
    in_tick = 0;
}
#endif   /* GAME_BOTW */
#endif   /* GAME_WONDER */
