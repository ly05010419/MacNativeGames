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
