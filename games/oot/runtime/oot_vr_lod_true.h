/* Ocarina of Time in VR: level of detail by the true distance to the eye (vr.md row 18; option "lod_true", default off).
   The parts without the runtime, the reasons and the site form are in zeldaVR/runtime/oot_vr_lod_true.h; this is the game
   side, included by oot_vr.h after the camera's own state. HookshotVR (GPL-3) gave the idea and the 193 places (its
   vr_game_hooks.py _VR_DEPTH, camera-ui.md C 3.7); every one was read again on update 131072's image (2026-11-27,
   scratch/runs/2026-11/oot-vr2/row18/analyze.py and harness.c): its word, the depth sum before it with the snapshot and
   sphere registers below, and x30 dead after it. 192 pass, main+0x3628350 does not (x30 is read later: ldr x9, [x30, #8] at
   main+0x3628d00), and the installer, which checks each again on the image it patches, refuses it.

   The snapshot is ours when its view matrix (+0xa8, 48 bytes) is exactly one we wrote at view +8 (oot_vr_camera): the game's
   snapshot builder at main+0x33a8c0 (pacibsp) copies 48 bytes from x1 to x0 +0xa8, +0xb8, +0xc8 (stur q0 at main+0x33a8f0..
   0x33a900), and its caller at main+0x339f64 passes x1 = view + 8 (add x1, x8, #8). The last four views we wrote are kept with
   the headset's true eye and how far the view was moved back for the game's sorting (first person), oot_lt_note_view.

   Mac only: oot_vr_install never runs in an iOS export (oot_vr_wanted refuses TOTK_EXPORT_IOS) or on the phone
   (game_patches_ns2.c calls it under !TARGET_OS_IPHONE). With the option off the two veneers skip the body after three
   words (the flag on the data page); with it on, every depth sum on the snapshot of one of our views takes the distance. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_lod_true.h"
int vr_oot_lod_true(void);   /* zeldaVR/gfx/totk_vr.m: the VR page's "detail by the distance to the eyes" (0 off) */

typedef struct { uint32_t off; uint8_t d, s, o; } oot_lt_site;
/* main + offset, D (fadd s<D>, s0, s3), snapshot register, sphere register; sorted */
static const oot_lt_site OOT_LT_SITES[] = {
    { 0x8413c, 0, 21, 22 }, { 0x1214d78, 0, 26, 19 }, { 0x3608e84, 0, 28, 25 }, { 0x3609210, 0, 28, 25 }, { 0x36095a8, 0, 28, 25 }, { 0x3609944, 0, 28, 25 },
    { 0x3609cc4, 0, 28, 25 }, { 0x360a050, 0, 28, 25 }, { 0x360a3f8, 0, 23, 19 }, { 0x360a7a4, 0, 23, 19 }, { 0x360ab24, 0, 28, 25 }, { 0x360aeb0, 0, 28, 25 },
    { 0x360b29c, 0, 22, 21 }, { 0x360b688, 0, 22, 21 }, { 0x360ba14, 0, 28, 25 }, { 0x360bda0, 0, 28, 25 }, { 0x360c148, 0, 23, 19 }, { 0x360c4f4, 0, 23, 19 },
    { 0x360c874, 0, 28, 25 }, { 0x360cc70, 0, 28, 25 }, { 0x360d078, 0, 28, 25 }, { 0x360d484, 0, 28, 25 }, { 0x360d874, 0, 28, 25 }, { 0x360dc70, 0, 28, 25 },
    { 0x360e088, 0, 19, 23 }, { 0x360e4b4, 0, 19, 23 }, { 0x360e8b4, 0, 28, 25 }, { 0x360ecb0, 0, 28, 25 }, { 0x360f110, 0, 22, 21 }, { 0x360f57c, 0, 22, 21 },
    { 0x360f974, 0, 28, 25 }, { 0x360fd70, 0, 28, 25 }, { 0x3610188, 0, 19, 23 }, { 0x36105b4, 0, 19, 23 }, { 0x36109b4, 0, 20, 28 }, { 0x3610d50, 0, 20, 28 },
    { 0x36110f8, 0, 20, 28 }, { 0x36114a4, 0, 20, 28 }, { 0x3611844, 0, 20, 28 }, { 0x3611be0, 0, 20, 28 }, { 0x3611fa0, 0, 23, 21 }, { 0x361238c, 0, 23, 21 },
    { 0x3612744, 0, 20, 28 }, { 0x3612ae0, 0, 20, 28 }, { 0x3612edc, 0, 21, 20 }, { 0x36132e8, 0, 21, 20 }, { 0x3613684, 0, 20, 28 }, { 0x3613a20, 0, 20, 28 },
    { 0x3613de0, 0, 23, 21 }, { 0x36141cc, 0, 23, 21 }, { 0x3614584, 0, 20, 28 }, { 0x36149a0, 0, 20, 28 }, { 0x3614dc8, 0, 20, 28 }, { 0x36151f4, 0, 20, 28 },
    { 0x3615604, 0, 20, 28 }, { 0x3615a20, 0, 20, 28 }, { 0x3615e58, 0, 19, 21 }, { 0x36162a4, 0, 19, 21 }, { 0x36166c4, 0, 20, 28 }, { 0x3616ae0, 0, 20, 28 },
    { 0x3616f64, 0, 21, 20 }, { 0x36173f0, 0, 21, 20 }, { 0x3617804, 0, 20, 28 }, { 0x3617c20, 0, 20, 28 }, { 0x3618058, 0, 19, 21 }, { 0x36184a4, 0, 19, 21 },
    { 0x36188f8, 0, 21, 26 }, { 0x3618e04, 0, 21, 26 }, { 0x361931c, 0, 21, 26 }, { 0x3619838, 0, 21, 26 }, { 0x3619d48, 0, 21, 26 }, { 0x361a254, 0, 21, 26 },
    { 0x361a768, 0, 23, 19 }, { 0x361aca0, 0, 11, 19 }, { 0x361b1b8, 0, 21, 26 }, { 0x361b6c4, 0, 21, 26 }, { 0x361bc10, 0, 20, 22 }, { 0x361c168, 0, 20, 22 },
    { 0x361c678, 0, 21, 26 }, { 0x361cb84, 0, 21, 26 }, { 0x361d098, 0, 23, 19 }, { 0x361d5ac, 0, 21, 26 }, { 0x361db24, 0, 21, 26 }, { 0x361e0c0, 0, 21, 26 },
    { 0x361e658, 0, 21, 26 }, { 0x361ebdc, 0, 21, 26 }, { 0x361f154, 0, 21, 26 }, { 0x361f6e8, 0, 21, 23 }, { 0x361fca8, 0, 11, 23 }, { 0x362023c, 0, 21, 26 },
    { 0x36207b4, 0, 21, 26 }, { 0x3620d80, 0, 20, 22 }, { 0x3621358, 0, 20, 22 }, { 0x36218ec, 0, 21, 26 }, { 0x3621e64, 0, 21, 26 }, { 0x36223f8, 0, 21, 23 },
    { 0x36229b8, 0, 11, 23 }, { 0x3622f44, 0, 21, 20 }, { 0x3623460, 0, 21, 20 }, { 0x3623988, 0, 21, 20 }, { 0x3623eb4, 0, 21, 20 }, { 0x36243d4, 0, 21, 20 },
    { 0x36248f0, 0, 21, 20 }, { 0x3624e10, 0, 19, 25 }, { 0x362535c, 0, 19, 25 }, { 0x36258a4, 0, 21, 20 }, { 0x3625dc0, 0, 21, 20 }, { 0x3626320, 0, 25, 21 },
    { 0x3626888, 0, 25, 21 }, { 0x3626da4, 0, 21, 20 }, { 0x36272c0, 0, 21, 20 }, { 0x36277e0, 0, 19, 25 }, { 0x3628350, 0, 10, 26 }, { 0x3629c54, 0, 21, 20 },
    { 0x362a200, 0, 21, 20 }, { 0x362a798, 0, 21, 20 }, { 0x362ad54, 0, 21, 20 }, { 0x362b2e4, 0, 21, 20 }, { 0x362b890, 0, 21, 20 }, { 0x362be38, 0, 21, 25 },
    { 0x362c414, 0, 21, 25 }, { 0x362c9d4, 0, 21, 20 }, { 0x362cf80, 0, 21, 20 }, { 0x362d560, 0, 25, 21 }, { 0x362db48, 0, 25, 21 }, { 0x362e0d4, 0, 21, 20 },
    { 0x362e680, 0, 21, 20 }, { 0x362ec28, 0, 21, 25 }, { 0x362f204, 0, 21, 25 }, { 0x362f928, 0, 20, 19 }, { 0x362ffd0, 0, 10, 24 }, { 0x363054c, 0, 10, 22 },
    { 0x3630c20, 0, 10, 24 }, { 0x3631170, 0, 20, 19 }, { 0x3631824, 0, 10, 24 }, { 0x3631dbc, 0, 10, 23 }, { 0x36324a4, 0, 10, 24 }, { 0x3632a68, 1, 19, 22 },
    { 0x363314c, 1, 8, 24 }, { 0x3633708, 1, 23, 22 }, { 0x3633e1c, 1, 8, 24 }, { 0x36343d8, 1, 19, 22 }, { 0x3634adc, 1, 8, 24 }, { 0x36350b8, 1, 23, 22 },
    { 0x36357ec, 1, 8, 24 }, { 0x3635f28, 0, 19, 28 }, { 0x36367a8, 0, 28, 22 }, { 0x3636f00, 0, 19, 24 }, { 0x36377b8, 0, 28, 22 }, { 0x3637f0c, 0, 19, 28 },
    { 0x3638818, 0, 28, 22 }, { 0x3638f90, 0, 19, 24 }, { 0x36398b8, 0, 28, 22 }, { 0x363a074, 0, 24, 28 }, { 0x363a980, 0, 9, 23 }, { 0x363b150, 0, 19, 24 },
    { 0x363ba80, 0, 9, 23 }, { 0x363c268, 0, 9, 28 }, { 0x363cbdc, 0, 8, 23 }, { 0x363d3d0, 0, 19, 24 }, { 0x363dd5c, 0, 8, 23 }, { 0x363e32c, 0, 10, 19 },
    { 0x363e9b8, 0, 10, 24 }, { 0x363ef08, 0, 19, 23 }, { 0x363f5b8, 0, 10, 24 }, { 0x363fafc, 0, 10, 19 }, { 0x36401a8, 0, 10, 24 }, { 0x3640728, 0, 19, 23 },
    { 0x3640df8, 0, 10, 24 }, { 0x364136c, 0, 10, 19 }, { 0x36419f8, 0, 10, 24 }, { 0x3641f48, 0, 19, 23 }, { 0x36425f8, 0, 10, 24 }, { 0x3642b3c, 0, 10, 19 },
    { 0x36431e8, 0, 10, 24 }, { 0x3643768, 0, 19, 23 }, { 0x3643e38, 0, 10, 24 }, { 0x3644558, 0, 19, 22 }, { 0x3644dd0, 0, 28, 24 }, { 0x3645524, 0, 19, 28 },
    { 0x3645dd0, 0, 28, 24 }, { 0x3646508, 0, 19, 22 }, { 0x3646e00, 0, 28, 24 }, { 0x3647574, 0, 19, 28 }, { 0x3647ea0, 0, 28, 24 }, { 0x36485f8, 0, 19, 22 },
    { 0x3648e70, 0, 28, 24 }, { 0x36495c4, 0, 19, 28 }, { 0x3649e70, 0, 28, 24 }, { 0x364a5a8, 0, 19, 22 }, { 0x364aea0, 0, 28, 24 }, { 0x364b614, 0, 19, 28 },
    { 0x364bf40, 0, 28, 24 },
};
enum { OOT_LT_COUNT = sizeof OOT_LT_SITES / sizeof *OOT_LT_SITES, OOT_LT_FLAG_CELL = 1024, OOT_LT_VIEWS = 4, OOT_LT_VISITED = 8192 };
static uintptr_t oot_lt_veneers;              /* the D = 0 veneer; the D = 1 one follows */
static _Atomic uint32_t *oot_lt_flag;          /* the data page's flag the veneers test */
static _Atomic uint64_t oot_lt_calls, oot_lt_ours, oot_lt_changed;

/* The views we wrote (seqlock: odd while it is being written). */
typedef struct { _Atomic uint32_t seq, m[12], true_z, eye[3], back; } oot_lt_view;
static oot_lt_view oot_lt_views[OOT_LT_VIEWS];
static unsigned oot_lt_next;                   /* the game's thread only */
/* oot_vr_camera, the game's thread: wrote = the matrix at view +8, m the true view (with the head). */
static void oot_lt_note_view(const float wrote[12], const float m[12]) {
    if (!oot_lt_flag) return;
    const int on = vr_oot_lod_true() != 0;
    if ((int)atomic_exchange(oot_lt_flag, (uint32_t)on) != on)
        fprintf(stderr, "[vr-lod] detail by %s\n", on ? "the distance to the eyes (the game's depth along the view is replaced)" : "the game's depth along the view");
    if (!on) return;
    float eye[3]; oot_lt_eye(m, eye);
    const float back = m[11] - wrote[11];      /* oot_cull_view lowers only the z translation */
    oot_lt_view *v = &oot_lt_views[oot_lt_next++ % OOT_LT_VIEWS];
    const uint32_t s = atomic_load_explicit(&v->seq, memory_order_relaxed);
    atomic_store_explicit(&v->seq, s + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    uint32_t bits;
    for (int i = 0; i < 12; i++) { memcpy(&bits, &wrote[i], 4); atomic_store_explicit(&v->m[i], bits, memory_order_relaxed); }
    for (int i = 0; i < 3; i++) { memcpy(&bits, &eye[i], 4); atomic_store_explicit(&v->eye[i], bits, memory_order_relaxed); }
    memcpy(&bits, &back, 4); atomic_store_explicit(&v->back, bits, memory_order_relaxed);
    memcpy(&bits, &m[11], 4); atomic_store_explicit(&v->true_z, bits, memory_order_relaxed);
    atomic_store_explicit(&v->seq, s + 2, memory_order_release);
}
/* The eye and the move back of the view whose matrix (bits) the snapshot holds; 0 when it is none of ours. The snapshot may
   hold the view as we wrote it, or with the true z translation put back (oot_vr_fill does that where the game fills a view
   for the GPU; whether that comes before the snapshot is not known): then nothing was moved back. */
static int oot_lt_find_view(const uint32_t snap[12], float eye[3], float *back) {
    for (unsigned k = 0; k < OOT_LT_VIEWS; k++) {
        oot_lt_view *v = &oot_lt_views[k];
        const uint32_t s = atomic_load_explicit(&v->seq, memory_order_acquire);
        if (!s || (s & 1)) continue;
        int same = 1;
        for (int i = 10; same && i >= 0; i--) same = atomic_load_explicit(&v->m[i], memory_order_relaxed) == snap[i];
        const uint32_t wrote_z = atomic_load_explicit(&v->m[11], memory_order_relaxed), true_z = atomic_load_explicit(&v->true_z, memory_order_relaxed);
        uint32_t e[3], b;
        for (int i = 0; i < 3; i++) e[i] = atomic_load_explicit(&v->eye[i], memory_order_relaxed);
        b = atomic_load_explicit(&v->back, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (!same || (snap[11] != wrote_z && snap[11] != true_z) || atomic_load_explicit(&v->seq, memory_order_relaxed) != s) continue;
        memcpy(eye, e, sizeof e);
        if (snap[11] == wrote_z) memcpy(back, &b, 4); else *back = 0;
        return 1;
    }
    return 0;
}
static const oot_lt_site *oot_lt_site_at(uint64_t off) {
    unsigned lo = 0, hi = OOT_LT_COUNT;
    while (lo < hi) { const unsigned mid = (lo + hi) / 2; if (OOT_LT_SITES[mid].off < off) lo = mid + 1; else hi = mid; }
    return lo < OOT_LT_COUNT && OOT_LT_SITES[lo].off == off ? &OOT_LT_SITES[lo] : NULL;
}
static void oot_lt_say(float was, float now) {   /* TOTK_VR_BLADELOG=1: once a second while the option is on */
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_BLADELOG"); on = e && *e == '1'; }
    if (!on) return;
    static _Atomic uint64_t at; const uint64_t t = oot_vr_now(), was_at = atomic_load(&at);
    if (t - was_at < 1000000000ull || !atomic_compare_exchange_strong(&at, &(uint64_t){was_at}, t)) return;
    fprintf(stderr, "[vr-lod] %llu depth sums so far, %llu on our views, %llu took the distance (this one: depth %.3f, now %.3f)\n",
            (unsigned long long)atomic_load(&oot_lt_calls), (unsigned long long)atomic_load(&oot_lt_ours),
            (unsigned long long)atomic_load(&oot_lt_changed), (double)-was, (double)-now);
}
/* The shared hook body's host, for a return address in one of the veneers (oot_vr_hook_dispatch). */
static int oot_lt_owns(uint64_t stub_return) {
    return oot_lt_veneers && (stub_return == oot_lt_veneers + OOT_LT_VENEER_RETURN * 4 ||
                              stub_return == oot_lt_veneers + (OOT_LT_VENEER_WORDS + OOT_LT_VENEER_RETURN) * 4);
}
/* The new depth for this site's sum, 0 when the game's stays. */
static int oot_lt_replace(const oot_hook_regs *r, const oot_lt_site *site, float z, float *now) {
    const uintptr_t snap = (uintptr_t)r->x[site->s], sphere = (uintptr_t)r->x[site->o];
    if (!snap || !sphere || !(z < 0)) return 0;
    uint32_t mat[12]; float c[3], eye[3], back;
    memcpy(mat, (const void *)(snap + 0xa8), sizeof mat);   /* the game read +0xc8.. of it a moment ago */
    if (!oot_lt_find_view(mat, eye, &back)) return 0;
    atomic_fetch_add_explicit(&oot_lt_ours, 1, memory_order_relaxed);
    memcpy(c, (const void *)sphere, sizeof c);              /* and the sphere's centre */
    return oot_lt_depth(z, c, eye, back, now);
}
static oot_guest_call *oot_lt_hook(oot_hook_regs *r) {
    if (!oot_lt_flag || !atomic_load_explicit(oot_lt_flag, memory_order_relaxed)) return NULL;
    atomic_fetch_add_explicit(&oot_lt_calls, 1, memory_order_relaxed);
    const oot_lt_site *site = oot_lt_site_at(r->x[30] - 4 - (uintptr_t)oot_vr_base);   /* bl at the site: x30 = its next word */
    if (!site) return NULL;
    float z, now; memcpy(&z, r->q[site->d], 4);
    if (oot_lt_replace(r, site, z, &now)) {
        memcpy(r->q[site->d], &now, 4);                     /* the fadd zeroed the rest of the register already */
        atomic_fetch_add_explicit(&oot_lt_changed, 1, memory_order_relaxed);
    } else now = z;
    oot_lt_say(z, now);
    return NULL;
}

/* oot_vr_hooks_install, after the other hooks: the two veneers at `at` (16-aligned), then each site checked and turned into
   bl <veneer>. limit: the end of the text padding; body: the shared hook body; page: the data page. Returns the padding's
   new end (at, untouched, when nothing is installed). */
static uint64_t oot_lt_install(Module *m, uint64_t at, uint64_t limit, uint64_t page, uint64_t body) {
    static uint32_t visited[OOT_LT_VISITED];
    const uintptr_t b = (uintptr_t)m->base;
    at = (at + 15) & ~15ull;
    const uint64_t end = at + 2 * OOT_LT_VENEER_WORDS * 4;
    if (end > limit || page + OOT_LT_FLAG_CELL + 4 > m->size) { fprintf(stderr, "[vr-lod] no room for the two veneers: detail stays by the game's depth\n"); return at; }
    for (uint64_t k = at; k < end; k++) if (m->base[k]) { fprintf(stderr, "[vr-lod] the veneers' room is in use: detail stays by the game's depth\n"); return at; }
    for (uint64_t k = page + OOT_LT_FLAG_CELL; k < page + OOT_LT_FLAG_CELL + 4; k++) if (m->base[k]) { fprintf(stderr, "[vr-lod] the data page's flag is in use: detail stays by the game's depth\n"); return at; }
    for (unsigned d = 0; d < 2; d++) {
        uint32_t w[OOT_LT_VENEER_WORDS];
        const char *e = oot_lt_veneer(d, b + at + d * sizeof w, b + body, b + page, OOT_LT_FLAG_CELL, w);
        if (e) { fprintf(stderr, "[vr-lod] %s: detail stays by the game's depth\n", e); return at; }
        memcpy(m->base + at + d * sizeof w, w, sizeof w);
    }
    oot_lt_veneers = b + at; oot_lt_flag = (_Atomic uint32_t *)(void *)(m->base + page + OOT_LT_FLAG_CELL);
    /* the code the paths may run: main's words, without its read-only data and data (between the text padding and the
       rewriter's patch area) */
    const uint32_t *code = (const uint32_t *)(const void *)m->base;
    const uint64_t nwords = m->file_size / 4, gap_lo = limit / 4, gap_hi = m->patch_off / 4;
    int installed = 0, refused = 0;
    for (unsigned k = 0; k < OOT_LT_COUNT; k++) {
        const oot_lt_site *s = &OOT_LT_SITES[k];
        const char *why = "";
        int ok = oot_lt_site_ok(code, gap_lo, s->off / 4, s->d, s->s, s->o, &why);
        if (ok) ok = oot_lt_x30_dead(code, nwords, gap_lo, gap_hi, s->off / 4, visited, OOT_LT_VISITED, 4096, &why);
        const uint32_t bl = ok ? oot_lt_site_bl(b + s->off, b + at + s->d * OOT_LT_VENEER_WORDS * 4) : 0;
        if (ok && !bl) why = "the veneer is out of branch range";
        if (!bl) {
            if (refused++ < 10) { uint32_t have; memcpy(&have, m->base + s->off, 4); fprintf(stderr, "[vr-lod] main+%#x refused (%08x): %s\n", s->off, have, why); }
            continue;
        }
        memcpy(m->base + s->off, &bl, 4);
        installed++;
    }
    fprintf(stderr, "[vr-lod] %d of %d depth sums take the distance to the eyes when the option is on, %d refused; %d bytes of text padding\n",
            installed, (int)OOT_LT_COUNT, refused, (int)(end - at));
    return end;
}
