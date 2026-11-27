/* Third person, the aim's mark where the game's own shot will land (vr.md row 73; HookshotVR's way, ideas and addresses only).
   The game decides a slingshot or bow shot from its own camera, and in VR the head turns that view: its flat mark in the middle of
   the picture is where the head looks, not where the shot goes. So the game itself is asked where the shot would go.
   The asking is the game's own: the shoot function (main+0x2e03560, read on our image) takes the aimer from the launch record's
   +0x1968 and calls the aimer's class function at +0x10 with x0 = the aimer, x1 = the velocity out, x2 = the point it flies to out,
   x3 = where it starts out, x4 = a flag byte out. Outside a shot the aimer is reached as HookshotVR found it: equipment slot 3 (the
   bow or slingshot) -> its components (+0x270) -> +0x180 the shooter -> +0x178 the aimer (the launch record's +0x1968 is the same).
   It answers only on the actors' threads: the VR side says it wants a point (runtime_oot_vr_aim_want), Link's next update asks,
   the update after reads the buffers back. Included by oot_vr.h, after oot_vr_sword.h. */
#pragma once

enum { OOT_AIM_SLOT = 3, OOT_AIM_SHOOTER_OF_COMPONENTS = 0x180, OOT_AIM_AIMER_OF_SHOOTER = 0x178, OOT_AIM_CLASS_FN = 0x10 };
static const uint64_t OOT_AIM_WANT_NS = 100000000ull;
static _Atomic uint64_t oot_aim_wanted;   /* when the VR side last wanted a point */
static struct { pthread_mutex_t mu; float point[3], start[3]; uint64_t at; } oot_aim = { .mu = PTHREAD_MUTEX_INITIALIZER };
static float oot_aim_vel[3], oot_aim_point[3], oot_aim_start[3];   /* the call's buffers: Link's update only */
static uint8_t oot_aim_flag;
static int oot_aim_asked;
static _Atomic uintptr_t oot_aim_components;   /* Link's components, from his update */

void runtime_oot_vr_aim_want(void) { atomic_store(&oot_aim_wanted, oot_vr_now()); }
/* The newest answer: the point the shot would fly to and where it starts, in the game's world; age_ns how old. */
int runtime_oot_vr_aim_point(float point[3], float start[3], uint64_t *age_ns) {
    pthread_mutex_lock(&oot_aim.mu);
    const uint64_t at = oot_aim.at;
    if (at) { memcpy(point, oot_aim.point, sizeof oot_aim.point); memcpy(start, oot_aim.start, sizeof oot_aim.start); }
    pthread_mutex_unlock(&oot_aim.mu);
    if (!at) return 0;
    *age_ns = oot_vr_now() - at;
    return 1;
}
static void oot_aim_note(uintptr_t components) { atomic_store_explicit(&oot_aim_components, components, memory_order_relaxed); }

/* Link's hit sensor update (oot_link_calc): read the last answer, and ask again while it is wanted. */
static oot_guest_call *oot_aim_calc(void) {
    static oot_guest_call call;
    if (oot_aim_asked) {   /* the call of the last update has run: its buffers hold the answer */
        oot_aim_asked = 0;
        int ok = 1;
        for (int i = 0; i < 3; i++) ok &= isfinite(oot_aim_point[i]) && isfinite(oot_aim_start[i]);
        ok &= oot_aim_point[0] != 0 || oot_aim_point[1] != 0 || oot_aim_point[2] != 0;
        if (ok) {
            pthread_mutex_lock(&oot_aim.mu);
            memcpy(oot_aim.point, oot_aim_point, sizeof oot_aim_point); memcpy(oot_aim.start, oot_aim_start, sizeof oot_aim_start); oot_aim.at = oot_vr_now();
            pthread_mutex_unlock(&oot_aim.mu);
            static int said; if (said++ < 3) fprintf(stderr, "[vr-aim] the game's own answer: from %.2f %.2f %.2f to %.2f %.2f %.2f\n", (double)oot_aim_start[0], (double)oot_aim_start[1],
                                                       (double)oot_aim_start[2], (double)oot_aim_point[0], (double)oot_aim_point[1], (double)oot_aim_point[2]);
        }
    }
    const uint64_t wanted = atomic_load(&oot_aim_wanted), now = oot_vr_now();
    if (!wanted || now - wanted > OOT_AIM_WANT_NS || !oot_vr_base) return NULL;
    const uintptr_t components = atomic_load_explicit(&oot_aim_components, memory_order_relaxed);
    if (!components) return NULL;
    uint8_t state = 0;
    const uintptr_t weapon = oot_vr_equip_actor(components, OOT_AIM_SLOT, &state);
    uint64_t parts = 0, shooter = 0, cls = 0, fn = 0;
    if (!weapon || !oot_vr_safe_read(weapon + 0x270, &parts, 8) || !parts || !oot_vr_safe_read(parts + OOT_AIM_SHOOTER_OF_COMPONENTS, &shooter, 8) || !shooter) return NULL;
    const uintptr_t aimer = (uintptr_t)shooter + OOT_AIM_AIMER_OF_SHOOTER;
    if (!oot_vr_safe_read(aimer, &cls, 8) || !cls || !oot_vr_safe_read(cls + OOT_AIM_CLASS_FN, &fn, 8) || !fn) return NULL;
    const uintptr_t base = (uintptr_t)oot_vr_base;
    if (fn < base || fn >= base + 0x3c47000) { static int said; if (!said++) fprintf(stderr, "[vr-aim] the aimer's function %#llx is not in the game's code: not asked\n", (unsigned long long)fn); return NULL; }
    memset(oot_aim_vel, 0, sizeof oot_aim_vel); memset(oot_aim_point, 0, sizeof oot_aim_point); memset(oot_aim_start, 0, sizeof oot_aim_start); oot_aim_flag = 0;
    call = (oot_guest_call){ fn, (uint64_t)aimer, (uint64_t)(uintptr_t)oot_aim_vel, (uint64_t)(uintptr_t)oot_aim_point, (uint64_t)(uintptr_t)oot_aim_start, 0, NULL,
                             (uint64_t)(uintptr_t)&oot_aim_flag };
    oot_aim_asked = 1;
    return &call;
}
