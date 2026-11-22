/* zeldaVR for Ocarina of Time on the Mac (2026-11-22, user: "开始开发 oot的vr mac版本 参考这个实现
   https://github.com/ReTracedGames/OcarinaVR"; the user's choices: third person by default, stereo later by depth
   reprojection). Only with TOTK_VR_CONTROL set, never in an iOS export. Included by game_patches_ns2.c after
   patch_oot_aspect (it uses oot_aspect_cell).

   Where the data is (update 131072 = 1.1.1, the version OcarinaVR / Hookshot bbdc370 name; checked against our image,
   vr研究.md 2026-11-22): the game builds its gameplay view in main+0x4c48c0; at main+0x4c5068 its 3x4 view matrix
   (rows R|t) has just been written at view +8 (x20 the view, vtable main+0x4bf8a88; x19 its camera). The gameplay view
   is camera 0's of the camera manager: [[main+0x4c86bc0]] -> +32 count, +40 list -> camera 0 -> +120. The matrix is
   computed afresh from the camera's eye and target every frame, so turning it does not feed back into the game's
   camera logic: the head's rotation h (rows of R^T, written by totk_vr.m) is applied here, as BotW's adapter does
   (games/totk/runtime/botw_vr_camera.h), and the game's culling, shadows and level of detail then follow the head.
   The field of view: the camera's vertical angle (camera +0x13c) is read for its projection at main+0x4c52f8; the
   veneer there takes the headset's angle from the data page (runtime_aspect_vr), the aspect from the existing aspect
   cell. Only ideas and addresses come from OcarinaVR (GPL-3); no code. */
#pragma once
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "oot_vr_veneer.h"
#include "../zeldaVR/runtime/vr_camera.h"
#include "../zeldaVR/runtime/vr_view_math.h"

enum {
    OOT_VR_FOV_CELL = 72,         /* data page: the headset's vertical field of view (float radians), 0 = the game's */
    OOT_VR_FN_CELL = 80,          /* data page: the camera call's host function (8 bytes) */
    OOT_VR_CALL_STUB = 512,       /* text padding after the time-step and resolution veneers (they end at +372) */
    OOT_VR_FOV_STUB = 512 + 256,
};
static const uint64_t OOT_VR_CAMERA_SITE = 0x4c5068, OOT_VR_FOV_SITE = 0x4c52f8;
static const uint32_t OOT_VR_CAMERA_WORD = 0xf9401268u;   /* ldr x8, [x19, #0x20] */
static const uint32_t OOT_VR_FOV_WORD = 0xbd413e60u;      /* ldr s0, [x19, #0x13c] */
static const uint64_t OOT_VR_VIEW_VTABLE = 0x4bf8a88, OOT_VR_CAMERA_MANAGER = 0x4c86bc0;

static vr_cam_state oot_vr_cam;
static uint8_t *oot_vr_base;
static _Atomic uint32_t *oot_vr_fov_cell;
static pthread_mutex_t oot_vr_mu = PTHREAD_MUTEX_INITIALIZER;
static float oot_vr_base_view[12];
static int oot_vr_base_valid;
static uint32_t oot_vr_aspect_saved;   /* the player's ratio while VR set its own */
static int oot_vr_aspect_owned;

static uint64_t oot_vr_load64(uintptr_t a) { uint64_t v; memcpy(&v, (const void *)a, 8); return v; }

/* The gameplay view: camera 0 of the camera manager (0 while there is none). */
static uintptr_t oot_vr_gameplay_view(void) {
    const uintptr_t singleton = oot_vr_load64((uintptr_t)oot_vr_base + OOT_VR_CAMERA_MANAGER);
    const uintptr_t manager = singleton ? oot_vr_load64(singleton) : 0;
    uint32_t count = 0;
    if (manager) memcpy(&count, (const void *)(manager + 32), 4);
    const uintptr_t cameras = manager && count ? oot_vr_load64(manager + 40) : 0;
    const uintptr_t camera = cameras ? oot_vr_load64(cameras) : 0;
    return camera ? oot_vr_load64(camera + 120) : 0;
}

int runtime_vr_base_view(float out[12]) {
    pthread_mutex_lock(&oot_vr_mu);
    const int valid = oot_vr_base_valid;
    if (valid) memcpy(out, oot_vr_base_view, sizeof oot_vr_base_view);
    pthread_mutex_unlock(&oot_vr_mu);
    return valid;
}

/* The game's thread, right after it wrote a view's matrix (the veneer at main+0x4c5068). */
static void oot_vr_camera(uint8_t *view, uint8_t *camera) {
    (void)camera;
    if (!oot_vr_base || !view) return;
    if (oot_vr_load64((uintptr_t)view) != (uintptr_t)oot_vr_base + OOT_VR_VIEW_VTABLE) return;   /* another kind of view */
    if ((uintptr_t)view != oot_vr_gameplay_view()) return;                                       /* not the gameplay camera */
    vr_cam_state control;
    vr_camera_control_read(&control);
    if (!control.on) {
        pthread_mutex_lock(&oot_vr_mu); oot_vr_base_valid = 0; pthread_mutex_unlock(&oot_vr_mu);
        return;
    }
    float m[12];
    memcpy(m, view + 8, sizeof m);
    for (int i = 0; i < 12; i++) if (!isfinite(m[i]) || !isfinite(control.h[i])) return;
    for (int i = 0; i < 3; i++) if (!isfinite(control.off[i])) return;
    if (!isfinite(control.cam_back)) return;
    if (control.level) vr_view_level(m);
    pthread_mutex_lock(&oot_vr_mu);
    memcpy(oot_vr_base_view, m, sizeof m); oot_vr_base_valid = 1;
    pthread_mutex_unlock(&oot_vr_mu);
    memcpy(oot_vr_cam.game_right, m, 16);
    for (int i = 0; i < 3; i++) m[4 * i + 3] -= vr_dot3(m + 4 * i, control.off);
    m[11] -= control.cam_back;
    vr_view_turn(m, control.h, m);
    memcpy(view + 8, m, sizeof m);
    /* The ring the replay matches each frame's camera block against (totk_vr.m vr_note_camera): this frame's head pose. */
    const uint32_t calls = __atomic_load_n(&oot_vr_cam.calls, __ATOMIC_RELAXED) + 1;
    memcpy(oot_vr_cam.ring[calls & 15].m, m, sizeof m);
    oot_vr_cam.ring[calls & 15].seq = control.seq;
    memset(oot_vr_cam.ring[calls & 15].pos, 0, sizeof oot_vr_cam.ring[calls & 15].pos);   /* third person */
    __atomic_store_n(&oot_vr_cam.used, control.seq, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_cam.cam, (uint64_t)(uintptr_t)view, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_cam.calls, calls, __ATOMIC_RELEASE);
    if (calls < 4)
        fprintf(stderr, "[vr] OOT gameplay view %p turned by the head (sequence %u), view t %.3f %.3f %.3f\n", (void *)view,
                control.seq, (double)m[3], (double)m[7], (double)m[11]);
}

/* totk_vr.m headlook_fov: the game renders the headset's view. a: the width / height ratio (0: back to the player's);
   k < 0: the vertical half-angle tangent itself (k > 0, a widening factor, is TotK's Vert+ and not offered here). */
int runtime_aspect_vr(float a, float k) {
    if (!oot_vr_fov_cell || !oot_aspect_cell) return 0;
    if (!(a > 0)) {
        atomic_store(oot_vr_fov_cell, 0);
        if (oot_vr_aspect_owned) { atomic_store(oot_aspect_cell, oot_vr_aspect_saved); oot_vr_aspect_owned = 0; }
        fprintf(stderr, "[vr] OOT field of view and aspect back to the game's\n");
        return 1;
    }
    if (!(k < 0) || !isfinite(a) || !isfinite(k)) return 0;
    const float fovy = 2 * atanf(-k);
    uint32_t bits;
    if (!oot_vr_aspect_owned) { oot_vr_aspect_saved = atomic_load(oot_aspect_cell); oot_vr_aspect_owned = 1; }
    memcpy(&bits, &a, 4); atomic_store(oot_aspect_cell, bits);
    memcpy(&bits, &fovy, 4); atomic_store(oot_vr_fov_cell, bits);
    fprintf(stderr, "[vr] OOT renders the headset's view: aspect %.4f, vertical %.1f degrees\n", (double)a, (double)(fovy * 57.2957795f));
    return 1;
}

static int oot_vr_wanted(void) {
    const char *e = getenv("TOTK_VR_CONTROL");
    return e && *e && !getenv("TOTK_EXPORT_IOS");
}

static void oot_vr_install(Module *m) {
    if (!oot_vr_wanted()) return;
    uint32_t have, fov_have;
    memcpy(&have, m->base + OOT_VR_CAMERA_SITE, 4);
    memcpy(&fov_have, m->base + OOT_VR_FOV_SITE, 4);
    if (have != OOT_VR_CAMERA_WORD) runtime_fail("OOT VR: the gameplay view site differs from verified update 131072");
    if (fov_have != OOT_VR_FOV_WORD) runtime_fail("OOT VR: the field-of-view load differs from verified update 131072");
    const uint64_t page = (m->file_size + 0x3fff) & ~0x3fffULL, rx_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    const uint64_t pad = (m->text_end + 15) & ~15ULL, call_stub = pad + OOT_VR_CALL_STUB, fov_stub = pad + OOT_VR_FOV_STUB;
    uint32_t call[OOT_VR_CALL_WORDS], fov[OOT_VR_FOV_WORDS];
    const char *e = oot_vr_call_veneer(have, (uintptr_t)m->base + call_stub, (uintptr_t)m->base + OOT_VR_CAMERA_SITE,
                                       (uintptr_t)m->base + page, OOT_VR_FN_CELL, call);
    if (!e) e = oot_vr_fov_veneer(fov_have, (uintptr_t)m->base + fov_stub, (uintptr_t)m->base + OOT_VR_FOV_SITE,
                                  (uintptr_t)m->base + page, OOT_VR_FOV_CELL, fov);
    if (e) runtime_fail(e);
    if (call_stub + sizeof call > fov_stub || fov_stub + sizeof fov > rx_end || page + OOT_VR_FN_CELL + 8 > m->size)
        runtime_fail("OOT VR: no room left for the veneers");
    for (uint64_t k = call_stub; k < fov_stub + sizeof fov; k++) if (m->base[k]) runtime_fail("OOT VR: text padding already in use");
    for (uint64_t k = page + OOT_VR_FOV_CELL; k < page + OOT_VR_FN_CELL + 8; k++) if (m->base[k]) runtime_fail("OOT VR: data page already in use");
    void *host = runtime_hle_wrap_named((void *)oot_vr_camera, "our OOT VR camera");
    memcpy(m->base + page + OOT_VR_FN_CELL, &host, 8);
    oot_vr_fov_cell = (_Atomic uint32_t *)(void *)(m->base + page + OOT_VR_FOV_CELL);
    memcpy(m->base + call_stub, call, sizeof call);
    memcpy(m->base + fov_stub, fov, sizeof fov);
    const uint32_t to_call = oot_vr_b(OOT_VR_CAMERA_SITE, call_stub), to_fov = oot_vr_b(OOT_VR_FOV_SITE, fov_stub);
    if (!to_call || !to_fov) runtime_fail("OOT VR: veneer out of branch range");
    memcpy(m->base + OOT_VR_CAMERA_SITE, &to_call, 4);
    memcpy(m->base + OOT_VR_FOV_SITE, &to_fov, 4);
    oot_vr_base = m->base;
    g_vr_headlook = &oot_vr_cam;
    fprintf(stderr, "[vr] OOT: the head turns the gameplay view (main+0x%llx), the headset's field of view at main+0x%llx\n",
            (unsigned long long)OOT_VR_CAMERA_SITE, (unsigned long long)OOT_VR_FOV_SITE);
}
