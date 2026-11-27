/* Ocarina of Time's bow and slingshot string in the player's hands, without the runtime (tests/test_oot_vr_string.c runs it).
   The behaviour follows HookshotVR's notes on its bow (GPL-3: facts and ideas, no code; aim-items.md 1.5, 1.9 and 2):
   - The string: the hand that does not hold the bow grabs it when its input (trigger, grip or the larger of the two, the VR
     page's choice) goes above 0.55 within 15 cm of where the string rests; pressed elsewhere it does nothing until it is let go
     (below 0.35) and pressed again. The draw is how far the hand is behind the rest along the aim; from 12 cm on the draw is
     committed (the game's own draw button is then held for it). Letting go after a commit is a shot: the line it left along is
     kept for 20 frames, because the game launches a frame or two later. Letting go before it is no shot. The string is out of
     the hand, the commit kept until a real let-go, while a menu is open, the hand is not tracked, or it is 1.5 m from the bow.
   - The aim: at rest from the string's rest through the handle; while the string is held and the hand is 15 cm or more from the
     handle, from the drawing hand through the handle.
   - The draw's rumble on the drawing hand: none up to 3 cm, then 0.08 + 0.72 x the share of the next 45 cm, pulses of 120 ms
     sent again when it changed by more than 0.06 or 90 ms passed.
   - The strings and straps: each string bone is turned about the tip it hangs from, and stretched along that line (0.25 to 5
     times), so that the middle of the string comes to where it should be; the pouch is turned about the fork the same way and
     moved there.
   - The nocked arrow: the game's own arrow whose place against the game's own bow root is that of an arrow on the string (its
     origin from -0.45 to 0.40 m along the bow's x, within 12 cm across, its shaft along the bow's -x within about 15 degrees),
     moved so that its notch, 0.41 m behind its origin along its -z, sits on the string and its shaft along the aim.
   - A held fire, ice or light arrow's effects: their particle camera block (vertex uniform block 10: the camera's place at 0x00,
     its forward at 0x10, the view at 0x40 and the projection at 0x80, the inverse view at 0x1c0, column-major) changed so that the
     particles the game placed at its own arrow are seen where the VR arrow is; the fire flame fades itself near the camera (alpha
     x clamp(0.4 x depth - 1, 0, 1)), so the view's depth is stretched and the projection's depth column shrunk by the same factor:
     the picture keeps its place and the fade comes out at the opacity asked for.
   Distances in the room are metres; the game's world is in game units, `scale` of them to the player's metre. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "oot_vr_ik.h"
#include "oot_vr_pose.h"

enum { OOT_STRING_HELD = 1, OOT_STRING_COMMITTED = 2 };
enum { OOT_STRING_EITHER = 0, OOT_STRING_TRIGGER = 1, OOT_STRING_GRIP = 2, OOT_STRING_NONE = 3 };   /* the VR page's "string hand input" (3: no string) */
enum { OOT_STRING_FREEZE_FRAMES = 20, OOT_STRING_ABSENT_FRAMES = 3 };
static const float OOT_STRING_PRESS = 0.55f, OOT_STRING_LET_GO = 0.35f, OOT_STRING_REACH = 0.15f, OOT_STRING_COMMIT = 0.12f,
                   OOT_STRING_OUT_OF_HAND = 1.5f, OOT_STRING_LINE = 0.15f;

static inline float oot_string_input(int mode, float trigger, float grip) {
    return mode == OOT_STRING_TRIGGER ? trigger : mode == OOT_STRING_GRIP ? grip : fmaxf(trigger, grip);
}
static inline float oot_string_dist(const float a[3], const float b[3]) {
    const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
static inline int oot_string_finite(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }

typedef struct {
    int bow_present;          /* the bow's or slingshot's model is in the player's hand this frame */
    int tracked;              /* the drawing hand is tracked */
    int menu;                 /* a menu or the item wheel is open */
    float input;              /* the drawing hand's input, by the option (oot_string_input) */
    float handle[3];          /* the bow hand's grip, world */
    float rest[3];            /* where the string rests on the bow, world */
    float hand[3];            /* the drawing hand, world */
    float scale;              /* game units per metre of the room */
} oot_string_in;

typedef struct {
    int down, held, committed, blocked, absent, freeze;
    float dir[3], frozen_dir[3], frozen_handle[3], last_dir[3];
    unsigned shots;
} oot_string_state;

typedef struct {
    int flags;                /* OOT_STRING_HELD, OOT_STRING_COMMITTED */
    int fired;                /* this frame the string was let go after a commit */
    int valid;                /* the aim below is known */
    float draw;               /* metres in the room */
    float dir[3], handle[3];  /* the line the shot leaves along (kept 20 frames after a shot) */
    float string_at[3];       /* where the middle of the string goes */
} oot_string_out;

static inline void oot_string_reset(oot_string_state *s) { const unsigned shots = s->shots; memset(s, 0, sizeof *s); s->shots = shots; }

/* One step a game frame. */
static inline void oot_string_step(oot_string_state *s, const oot_string_in *in, oot_string_out *o) {
    memset(o, 0, sizeof *o);
    if (!in->bow_present) {
        if (++s->absent > OOT_STRING_ABSENT_FRAMES) oot_string_reset(s);
        return;
    }
    s->absent = 0;
    if (!oot_string_finite(in->handle, 3) || !oot_string_finite(in->rest, 3) || !oot_string_finite(in->hand, 3) || !(in->scale > 0.01f && in->scale < 100)) return;
    float rest_dir[3];
    if (!oot_unit(in->rest, in->handle, rest_dir)) return;
    const int was = s->down;
    if (in->input > OOT_STRING_PRESS) s->down = 1; else if (in->input < OOT_STRING_LET_GO) s->down = 0;
    const int pressed = s->down && !was;
    const float near_rest = oot_string_dist(in->hand, in->rest) / in->scale, from_bow = oot_string_dist(in->hand, in->handle) / in->scale;
    const int usable = in->tracked && !in->menu;
    if (s->blocked && !s->down) s->blocked = 0;
    if (!s->held && pressed) {
        if (usable && near_rest <= OOT_STRING_REACH && !s->committed) s->held = 1; else s->blocked = 1;
    }
    if (s->held && (!usable || from_bow > OOT_STRING_OUT_OF_HAND)) s->held = 0;   /* out of the hand: the commit waits for a real let-go */
    if (usable && !s->down && (s->held || s->committed)) {   /* a real let-go: tracked and no menu, also once a menu closes again */
        if (s->committed) {
            o->fired = 1; s->shots++; s->freeze = OOT_STRING_FREEZE_FRAMES;
            memcpy(s->frozen_handle, in->handle, 12); memcpy(s->frozen_dir, s->held ? s->dir : s->last_dir, 12);
        }
        s->held = 0; s->committed = 0;
    }
    float draw = 0;
    if (s->held) {
        const float back[3] = { in->rest[0] - in->hand[0], in->rest[1] - in->hand[1], in->rest[2] - in->hand[2] };
        draw = fmaxf(0, oot_ik_dot(back, rest_dir)) / in->scale;
        if (draw >= OOT_STRING_COMMIT) s->committed = 1;
    }
    float dir[3]; memcpy(dir, rest_dir, 12);
    if (s->held && from_bow >= OOT_STRING_LINE) { float line[3]; if (oot_unit(in->hand, in->handle, line)) memcpy(dir, line, 12); }
    memcpy(s->dir, dir, 12);
    if (s->held || !s->committed) memcpy(s->last_dir, dir, 12);
    o->flags = (s->held ? OOT_STRING_HELD : 0) | (s->committed ? OOT_STRING_COMMITTED : 0);
    o->draw = draw; o->valid = 1;
    if (s->freeze > 0) { s->freeze--; memcpy(o->dir, s->frozen_dir, 12); memcpy(o->handle, s->frozen_handle, 12); }
    else { memcpy(o->dir, dir, 12); memcpy(o->handle, in->handle, 12); }
    memcpy(o->string_at, s->held ? in->hand : in->rest, 12);
}

/* The draw's rumble. 1: send a pulse of 120 ms at *amplitude now. */
typedef struct { float sent; uint64_t at; } oot_string_buzz;
static inline float oot_string_rumble(float draw) {
    if (!(draw > 0.03f)) return 0;
    return 0.08f + 0.72f * fminf(1, fmaxf(0, (draw - 0.03f) / 0.45f));
}
static inline int oot_string_buzz_step(oot_string_buzz *b, float draw, uint64_t now, float *amplitude) {
    const float a = oot_string_rumble(draw);
    if (a <= 0) { b->sent = 0; return 0; }
    if (b->sent > 0 && fabsf(a - b->sent) <= 0.06f && now - b->at < 90000000ull) return 0;
    b->sent = a; b->at = now; *amplitude = a;
    return 1;
}

/* A string bone hanging from `tip`: the move that turns the line tip -> from onto tip -> to and stretches it to that length. */
static inline int oot_string_swing(const float tip[3], const float from[3], const float to[3], float out[12]) {
    float a[3], b[3];
    const float la = oot_string_dist(from, tip), lb = oot_string_dist(to, tip);
    if (!(la > 1e-4f) || !(lb > 1e-4f) || !oot_unit(tip, from, a) || !oot_unit(tip, to, b)) return 0;
    float l[12], r[12];
    oot_m_identity(l); oot_m_stretch(l, a, fminf(5, fmaxf(0.25f, lb / la)));
    oot_m_between(a, b, r); oot_m_mul(r, l, l);
    oot_m_about(l, tip, tip, out);
    return 1;
}
/* The pouch: turned about the fork as the line fork -> from turns onto fork -> to, and moved from `from` to `to`. */
static inline int oot_string_pouch(const float fork[3], const float from[3], const float to[3], float out[12]) {
    float a[3], b[3], r[12];
    if (!oot_unit(fork, from, a) || !oot_unit(fork, to, b)) return 0;
    oot_m_between(a, b, r);
    oot_m_about(r, from, to, out);
    return 1;
}

/* A 3x4's inverse (any invertible 3x3, not only a rotation). 0: none. */
static inline int oot_string_inverse(const float m[12], float out[12]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!isfinite(det) || fabsf(det) < 1e-9f) return 0;
    const float r[9] = { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                         (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                         (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det };
    for (int k = 0; k < 3; k++) {
        out[4 * k] = r[3 * k]; out[4 * k + 1] = r[3 * k + 1]; out[4 * k + 2] = r[3 * k + 2];
        out[4 * k + 3] = -(r[3 * k] * m[3] + r[3 * k + 1] * m[7] + r[3 * k + 2] * m[11]);
    }
    return oot_string_finite(out, 12);
}

/* The game's arrow sits on the game's own bow: bow the bow's root matrix this frame, arrow the arrow's, world (3x4 each). */
static inline int oot_arrow_nocked(const float bow[12], const float arrow[12]) {
    float inv[12], rel[12];
    if (!oot_string_finite(bow, 12) || !oot_string_finite(arrow, 12) || !oot_string_inverse(bow, inv)) return 0;
    oot_m_mul(inv, arrow, rel);
    float shaft[3] = { rel[2], rel[6], rel[10] };
    if (!oot_ik_norm(shaft)) return 0;
    return rel[3] > -0.45f && rel[3] < 0.40f && fabsf(rel[7]) < 0.12f && fabsf(rel[11]) < 0.12f && -shaft[0] > 0.9f;
}
/* The arrow put on the VR string: its notch (0.41 behind its origin along its own -z, in its own units) at `nock`, its shaft
   (+z) along `dir`, its -y along the bow's limbs (`limbs`, the bow's +z); its size is the game's (game: its matrix). */
static const float OOT_ARROW_NOTCH = 0.41f;
static inline int oot_arrow_place(const float game[12], const float nock[3], const float dir[3], const float limbs[3], float out[12]) {
    if (!oot_string_finite(game, 12) || !oot_string_finite(nock, 3) || !oot_string_finite(dir, 3) || !oot_string_finite(limbs, 3)) return 0;
    float z[3] = { dir[0], dir[1], dir[2] }, y[3], x[3];
    if (!oot_ik_norm(z)) return 0;
    const float k = oot_ik_dot(limbs, z);
    for (int i = 0; i < 3; i++) y[i] = -(limbs[i] - z[i] * k);
    if (!oot_ik_norm(y)) return 0;
    oot_ik_cross(y, z, x);
    float size[3];
    for (int c = 0; c < 3; c++) size[c] = sqrtf(game[c] * game[c] + game[4 + c] * game[4 + c] + game[8 + c] * game[8 + c]);
    if (!(size[2] > 1e-4f)) return 0;
    for (int i = 0; i < 3; i++) {
        out[4 * i] = x[i] * size[0]; out[4 * i + 1] = y[i] * size[1]; out[4 * i + 2] = z[i] * size[2];
        out[4 * i + 3] = nock[i] + z[i] * OOT_ARROW_NOTCH * size[2];
    }
    return 1;
}

/* ---- A held arrow's effects: the particle camera block (132 floats, 0x210 bytes, column-major 4x4s). ---- */
enum { OOT_FX_CAMERA_FLOATS = 0x210 / 4, OOT_FX_VIEW = 0x40 / 4, OOT_FX_PROJECTION = 0x80 / 4, OOT_FX_INVERSE_VIEW = 0x1c0 / 4 };
static const float OOT_FX_REACH = 0.8f, OOT_FX_CAMERA_MARGIN = 0.04f, OOT_FX_WHOLE = 10.0f;
static inline int oot_fx_close(float a, float b, float tolerance) { return isfinite(a) && isfinite(b) && fabsf(a - b) <= tolerance; }
static inline void oot_fx_mul4(const float a[16], const float b[16], float out[16]) {   /* column-major: out = a b */
    float c[16];
    for (int col = 0; col < 4; col++) for (int row = 0; row < 4; row++) {
        float s = 0; for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[col * 4 + k];
        c[col * 4 + row] = s;
    }
    memcpy(out, c, sizeof c);
}
/* A column-major affine 4x4 (last row 0 0 0 1) as a row-major 3x4, and back. */
static inline int oot_fx_to34(const float m[16], float out[12]) {
    if (!oot_string_finite(m, 16) || !oot_fx_close(m[3], 0, 1e-3f) || !oot_fx_close(m[7], 0, 1e-3f) || !oot_fx_close(m[11], 0, 1e-3f) || !oot_fx_close(m[15], 1, 1e-3f)) return 0;
    for (int row = 0; row < 3; row++) for (int col = 0; col < 4; col++) out[4 * row + col] = m[col * 4 + row];
    return 1;
}
static inline void oot_fx_to44(const float m[12], float out[16]) {
    for (int col = 0; col < 4; col++) { for (int row = 0; row < 3; row++) out[col * 4 + row] = m[4 * row + col]; out[col * 4 + 3] = col == 3 ? 1.0f : 0.0f; }
}
/* An effect whose matrix (column-major 4x4) sits within reach of the game's held arrow and is not the camera itself. */
static inline int oot_fx_source(const float effect[16], const float game_arrow[3], const float eye[3]) {
    float m[12];
    if (!oot_fx_to34(effect, m) || !oot_string_finite(game_arrow, 3) || !oot_string_finite(eye, 3)) return 0;
    const float at[3] = { m[3], m[7], m[11] };
    return oot_string_dist(at, game_arrow) <= OOT_FX_REACH && oot_string_dist(at, eye) > OOT_FX_CAMERA_MARGIN;
}
/* The camera's place, from the block (its inverse view). 0: the block is not a perspective camera. */
static inline int oot_fx_eye(const float c[OOT_FX_CAMERA_FLOATS], float eye[3]) {
    const float *p = c + OOT_FX_PROJECTION;
    if (!oot_string_finite(p, 16) || !oot_fx_close(p[3], 0, 1e-3f) || !oot_fx_close(p[7], 0, 1e-3f) || !oot_fx_close(fabsf(p[11]), 1, 1e-3f) ||
        !oot_fx_close(p[15], 0, 1e-3f) || fabsf(p[0]) < 1e-5f || fabsf(p[5]) < 1e-5f) return 0;
    float v[12], iv[12];
    if (!oot_fx_to34(c + OOT_FX_VIEW, v) || !oot_string_inverse(v, iv)) return 0;
    eye[0] = iv[3]; eye[1] = iv[7]; eye[2] = iv[11];
    return 1;
}
/* The view's depth stretch that makes the fade come out at `opacity` (0..1) at `depth` units in front of the camera; whole: 10. */
static inline float oot_fx_stretch(float depth, float opacity) {
    if (!(opacity < 1.0f) || !(depth > 1e-3f)) return OOT_FX_WHOLE;
    return (1.0f + fmaxf(opacity, 0.0f)) / (0.4f * depth);
}
/* The block changed so that a particle the game placed at p is seen at move(p): view V' = V move; its place, forward and inverse
   view by move's inverse (each only where the block agrees with V's inverse); then the depth stretch for the opacity, the effect
   at `effect_at` (world). 0: not changed. */
static inline int oot_fx_camera(float c[OOT_FX_CAMERA_FLOATS], const float move[12], const float effect_at[3], float opacity) {
    float eye[3], inv[12], v[12], iv0[12];
    if (!oot_fx_eye(c, eye) || !oot_string_finite(move, 12) || !oot_string_inverse(move, inv) || !oot_fx_to34(c + OOT_FX_VIEW, v) || !oot_string_inverse(v, iv0)) return 0;
    float d[16]; oot_fx_to44(move, d);
    oot_fx_mul4(c + OOT_FX_VIEW, d, c + OOT_FX_VIEW);
    if (oot_fx_close(c[0], eye[0], 0.02f) && oot_fx_close(c[1], eye[1], 0.02f) && oot_fx_close(c[2], eye[2], 0.02f)) {
        float moved[3]; oot_m_apply(inv, eye, moved); memcpy(c, moved, 12);
        float f[3] = { inv[0] * c[4] + inv[1] * c[5] + inv[2] * c[6], inv[4] * c[4] + inv[5] * c[5] + inv[6] * c[6], inv[8] * c[4] + inv[9] * c[5] + inv[10] * c[6] };
        if (oot_ik_norm(f)) memcpy(c + 4, f, 12);
    }
    {   /* the inverse view, where it is the inverse of the old view */
        float iv44[16]; oot_fx_to44(iv0, iv44); int same = 1;
        for (int i = 0; i < 16; i++) same &= oot_fx_close(c[OOT_FX_INVERSE_VIEW + i], iv44[i], 0.02f);
        if (same) { float i44[16]; oot_fx_to44(inv, i44); oot_fx_mul4(i44, c + OOT_FX_INVERSE_VIEW, c + OOT_FX_INVERSE_VIEW); }
    }
    float *vw = c + OOT_FX_VIEW, *p = c + OOT_FX_PROJECTION;
    const float depth = fabsf(vw[2] * effect_at[0] + vw[6] * effect_at[1] + vw[10] * effect_at[2] + vw[14]);
    const float k = oot_fx_stretch(depth, opacity);
    for (int col = 0; col < 4; col++) vw[col * 4 + 2] *= k;
    for (int row = 0; row < 4; row++) p[2 * 4 + row] /= k;
    return 1;
}
