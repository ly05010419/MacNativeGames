/* What the player holds is not cut away by the game's near plane (vr.md row 19), and a check of what the light and shadow see
   (row 16). The rescue is installed only with TOTK_VR_NEAR=1: our culling view is moved back an eye height already (oot_vr.h
   oot_cull_view), and batch2 (2026-11-27, the sword swept to 6 cm from the eyes) counted 1,043 tests of the held models here and
   not one that the near plane dropped. HookshotVR's way, ideas and addresses only (GPL-3); every site is checked against our image at install.

   Near plane. The game drops a model whose bounding sphere lies in front of its view's near plane: in the per-model test at
   main+0x7bb80, two branches load the near value `ldr s3, [x9, #0x1c8]` (main+0x7f47c, main+0x7df54) with x19 the model, x9 the
   view's snapshot, x25 the sphere (x, y, z, radius) and s1 the model's depth along the view (negative ahead); the model is dropped
   when near - radius > -z. In first person the sword and shield in the player's hand come closer to the eyes than the game ever
   puts them, and blink out. Right after the load, for those two models only, and only for the headset's own snapshot (its +0x1d0
   is the headset's vertical angle), a near the native test drops but -1.5 m would keep becomes -1.5 m for this one test. The
   snapshot itself is never written: depth, fog and other views read it.
   The models: the actor of equipment slot 0 (the sword) and 4 (the shield), each through its components +0x8 (a component of
   class main+0x4c61020) +0x18 (a resource, main+0x4c088a0) +0x18 (a node) whose first word is the model (class main+0x4c08cc8);
   read in Link's update while the game has the weapon in his hand, stale after 250 ms.

   Light and shadow (TOTK_VR_BLADELOG=1, log only). Two readers take tan(half the vertical angle) from the snapshot's +0x1dc
   (main+0x335ad0 into s0, main+0x6bc924 into s8). HookshotVR writes its foliage reference into +0x1dc and so had to give these
   readers the headset's own value; we leave +0x1dc alone. The log says, once a second, what they read against the headset's
   tan(half angle): equal means the light and shadow already see the headset's projection. Included by oot_vr.h. */
#pragma once

static const uint64_t OOT_MODEL_COMPONENT_CLASS = 0x4c61020, OOT_MODEL_RESOURCE_CLASS = 0x4c088a0, OOT_MODEL_CLASS = 0x4c08cc8;
static const uint64_t OOT_NEAR_FRESH_NS = 250000000ull;
static const float OOT_NEAR_HELD_M = -1.5f;
enum { OOT_NEAR_KEPT = 2 };
static _Atomic uintptr_t oot_near_kept[OOT_NEAR_KEPT];
static _Atomic uint64_t oot_near_kept_at, oot_near_rescued, oot_near_tested;

/* An actor's model, 0 when any link of the chain is not of the class it should be. */
static uintptr_t oot_actor_model(uintptr_t actor) {
    const uintptr_t base = (uintptr_t)oot_vr_base;
    uint64_t comps = 0, comp = 0, res = 0, node = 0, model = 0, cls = 0;
    if (!oot_vr_safe_read(actor + 0x270, &comps, 8) || !comps) return 0;
    if (!oot_vr_safe_read(comps + 0x8, &comp, 8) || !comp || !oot_vr_safe_read(comp, &cls, 8) || cls != base + OOT_MODEL_COMPONENT_CLASS) return 0;
    if (!oot_vr_safe_read(comp + 0x18, &res, 8) || !res || !oot_vr_safe_read(res, &cls, 8) || cls != base + OOT_MODEL_RESOURCE_CLASS) return 0;
    if (!oot_vr_safe_read(res + 0x18, &node, 8) || !node || !oot_vr_safe_read(node, &model, 8) || !model) return 0;
    if (!oot_vr_safe_read(model, &cls, 8) || cls != base + OOT_MODEL_CLASS) return 0;
    return (uintptr_t)model;
}
/* Link's update: his sword's and shield's models while the weapon is out. */
static void oot_near_note(uintptr_t components) {
    if (!oot_vr_base || !runtime_oot_vr_weapon_out()) { for (int i = 0; i < OOT_NEAR_KEPT; i++) atomic_store(&oot_near_kept[i], 0); return; }
    static const unsigned slots[OOT_NEAR_KEPT] = { 0, 4 };
    for (int i = 0; i < OOT_NEAR_KEPT; i++) {
        uint8_t state = 0; const uintptr_t actor = oot_vr_equip_actor(components, slots[i], &state);
        const uintptr_t model = actor ? oot_actor_model(actor) : 0;
        if (atomic_exchange(&oot_near_kept[i], model) != model && model)
            fprintf(stderr, "[vr-near] the %s's model %#llx is kept from the near plane\n", i ? "shield" : "sword", (unsigned long long)model);
    }
    atomic_store(&oot_near_kept_at, oot_vr_now());
}
/* The snapshot is the headset's: its vertical angle is the one the headset set (oot_vr_fov_cell). */
static int oot_snapshot_is_headsets(uintptr_t snapshot) {
    float fov = 0, vr = 0; uint32_t bits = oot_vr_fov_cell ? atomic_load(oot_vr_fov_cell) : 0;
    memcpy(&vr, &bits, 4);
    return bits && snapshot && oot_vr_safe_read(snapshot + 0x1d0, &fov, 4) && fabsf(fov - vr) < 0.01f;
}
/* main+0x7f47c, main+0x7df54, after `ldr s3, [x9, #0x1c8]`. */
static oot_guest_call *oot_near_test(oot_hook_regs *r) {
    if (!atomic_load(&oot_vr_fp_on)) return NULL;
    const uintptr_t model = (uintptr_t)r->x[19];
    int kept = 0;
    for (int i = 0; i < OOT_NEAR_KEPT; i++) if (model && atomic_load(&oot_near_kept[i]) == model) kept = 1;
    if (!kept || oot_vr_now() - atomic_load(&oot_near_kept_at) > OOT_NEAR_FRESH_NS) return NULL;
    if (!oot_snapshot_is_headsets((uintptr_t)r->x[9])) {   /* the log: a held model tested in a view that is not the headset's */
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 5) { float fov = 0; oot_vr_safe_read((uintptr_t)r->x[9] + 0x1d0, &fov, 4);
            fprintf(stderr, "[vr-near] a held model tested in another view (its angle %.4f)\n", (double)fov); }
        return NULL;
    }
    float near_v, z, radius = 0;
    memcpy(&near_v, r->q[3], 4); memcpy(&z, r->q[1], 4);
    if (!oot_vr_safe_read((uintptr_t)r->x[25] + 0xc, &radius, 4) || !isfinite(radius) || !isfinite(z) || !isfinite(near_v)) return NULL;
    {   /* the log: how often the game tests a held model here, once a second */
        const uint64_t n = atomic_fetch_add_explicit(&oot_near_tested, 1, memory_order_relaxed) + 1;
        static _Atomic uint64_t at; const uint64_t now = oot_vr_now(), was = atomic_load(&at);
        if (now - was >= 1000000000ull && atomic_compare_exchange_strong(&at, &(uint64_t){was}, now))
            fprintf(stderr, "[vr-near] %llu tests of a held model so far, %llu kept from the near plane (this one: depth %.3f, radius %.3f, near %.3f)\n",
                    (unsigned long long)n, (unsigned long long)atomic_load(&oot_near_rescued), (double)-z, (double)radius, (double)near_v);
    }
    if (!(near_v - radius > -z) || OOT_NEAR_HELD_M - radius > -z) return NULL;   /* kept anyway, or too far behind even so */
    memcpy(r->q[3], &OOT_NEAR_HELD_M, 4);
    const uint64_t n = atomic_fetch_add_explicit(&oot_near_rescued, 1, memory_order_relaxed) + 1;
    if (n <= 5 || !(n & (n - 1))) fprintf(stderr, "[vr-near] %.3f a held model kept from the near plane (%llu so far; depth %.3f, radius %.3f, near %.3f)\n",
                                          (double)oot_vr_now() / 1e9, (unsigned long long)n, (double)-z, (double)radius, (double)near_v);
    return NULL;
}
/* The light and shadow readers (log only). */
static void oot_light_tan_say(int which, float got) {
    static _Atomic uint64_t at[2];
    const uint64_t now = oot_vr_now(), was = atomic_load(&at[which]);
    if (now - was < 1000000000ull || !atomic_compare_exchange_strong(&at[which], &(uint64_t){was}, now)) return;
    float vr = 0; uint32_t bits = oot_vr_fov_cell ? atomic_load(oot_vr_fov_cell) : 0; memcpy(&vr, &bits, 4);
    fprintf(stderr, "[vr-light] reader %d read tan(half angle) %.4f; the headset's is %.4f (%s)\n", which, (double)got, (double)(bits ? tanf(vr / 2) : 0),
            bits && fabsf(got - tanf(vr / 2)) < 0.002f ? "the same" : "different");
}
static oot_guest_call *oot_light_tan_0(oot_hook_regs *r) { float v; memcpy(&v, r->q[0], 4); oot_light_tan_say(0, v); return NULL; }
static oot_guest_call *oot_light_tan_1(oot_hook_regs *r) { float v; memcpy(&v, r->q[8], 4); oot_light_tan_say(1, v); return NULL; }
