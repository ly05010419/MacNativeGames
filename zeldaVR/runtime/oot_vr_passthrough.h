/* zeldaVR, Ocarina of Time: the room around the screen (vr.md row 4). HookshotVR offers "around the window: dark, or your room
   where the headset's runtime offers passthrough" (its vr_options.inc vr_window_room, default 0 dark; it draws the room with
   XR_FB_passthrough under its layers; ideas only). Pure logic, no Metal, unit-tested (tests/test_oot_vr_passthrough.c).

   We cannot add a layer: our pictures reach the Quest as H.264 / HEVC video through basalt-streamer (zeldaVR/streamer/basaltvr.patch)
   and the ALVR 20.14.0 client (zeldaVR/install_vr.sh ALVR_VERSION), and the video has no alpha. That client has a chroma key
   (alvr/session/src/settings.rs VideoConfig.passthrough: Switch<PassthroughMode>, PassthroughMode::RgbChromaKey; its picture is
   keyed in alvr/graphics/resources/stream.wgsl chroma_key_mask, after the sRGB decode, ENABLE_SRGB_CORRECTION on a Quest, encoding
   gamma 1.0 from client_openxr/src/lib.rs). So:
     - the compositor draws everything around the screens in exactly the key colour (OOT_PT_KEY_*);
     - everything it draws keeps a dark border of at least OOT_PT_BORDER_PX pixels: the codec's colour smear at an edge then mixes
       the key with dark, which is far from the key, never with the picture (no fringes of the room inside the picture, no key
       coloured halo);
     - any colour of the picture itself the key would reach is pushed out of its reach (oot_pt_guard), so the room never shows
       through the game's own pinks and purples, nor in the head view while the key is still on (OOT_PT_HOLD_NS below).
   Key colour: magenta, not ALVR's default green (0, 255, 0): Ocarina of Time is full of bright greens (Kokiri Forest, Hyrule Field)
   that ALVR's green key reaches after the sRGB decode (the unit test shows grass keyed), and nearly no saturated magenta.
   Threshold 64 of ALVR's 1..255 (default 85): tighter, so fewer picture colours need the push; a flat key field stays well inside it
   after the codec. Feathering ALVR's default 0.05. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_PT_KEY_R = 255, OOT_PT_KEY_G = 0, OOT_PT_KEY_B = 255, OOT_PT_THRESHOLD = 64, OOT_PT_FEATHER_PCT = 5, OOT_PT_BORDER_PX = 6 };
#define OOT_PT_HOLD_NS   300000000ull   /* the key stays on this long after the last keyed frame: frames already on their way still show
                                           the key colour (the stream's latency, 67-131 ms measured, totk_vr.m stream_eye_size) */
#define OOT_PT_RESEND_NS 1000000000ull  /* while on, said again each second (as ALVR's own server sends its RealTimeConfig,
                                           server_core/src/connection.rs REAL_TIME_UPDATE_INTERVAL) */
#define OOT_PT_REACH     1.5f           /* a picture colour within this many thresholds of the key on every channel is pushed out */

/* ALVR stream.wgsl: sRGB to linear (THRESHOLD 0.04045, 1/12.92, 1/1.055, 2.4) */
static inline float oot_pt_lin(float s) { return s < 0.04045f ? s * 0.0773993808f : powf((s + 0.055f) * 0.94786729857f, 2.4f); }
static inline float oot_pt_srgb(float l) {
    if (!(l > 0)) return 0;
    if (l >= 1) return 1;
    return l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1 / 2.4f) - 0.055f;
}
static inline float oot_pt_smoothstep(float lo, float hi, float x) {   /* WGSL smoothstep(low, high, x), low > high allowed */
    float t = (x - lo) / (hi - lo); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t);
}
/* What ALVR keeps of a decoded colour (sRGB 0..1): 1 the picture, 0 the room (chroma_key_mask; the range per channel is
   key +- threshold * (1 -+ feathering), alvr/graphics/src/stream.rs PassthroughMode::RgbChromaKey). */
static inline float oot_pt_alvr_mask(const float srgb[3], const uint8_t key[3], int threshold, int feather_pct) {
    const float t = threshold / 255.0f, up = 1 + feather_pct / 100.0f, down = 1 - feather_pct / 100.0f;
    float m = 0;
    for (int c = 0; c < 3; c++) {
        const float k = key[c] / 255.0f, x = oot_pt_lin(srgb[c]);
        const float a = oot_pt_smoothstep(k - t * down, k - t * up, x), b = oot_pt_smoothstep(k + t * down, k + t * up, x);
        m = fmaxf(m, fmaxf(a, b));
    }
    return m;
}
static inline void oot_pt_key(uint8_t key[3]) { key[0] = OOT_PT_KEY_R; key[1] = OOT_PT_KEY_G; key[2] = OOT_PT_KEY_B; }

/* A picture colour (sRGB 0..1) moved out of the key's reach: when every channel is within OOT_PT_REACH thresholds of the key (in
   ALVR's linear space), the channel that needs the least change goes to that distance, away from the key. Anything else is left as
   it is. The compositor's key pass (oot_vr_passthrough_draw.h) does the same in its shader. 1: it was moved. */
static inline int oot_pt_guard(float srgb[3]) {
    uint8_t key[3]; oot_pt_key(key);
    const float reach = OOT_PT_REACH * OOT_PT_THRESHOLD / 255.0f;
    int best = -1; float best_d = 0, best_to = 0;
    for (int c = 0; c < 3; c++) {
        const float k = key[c] / 255.0f, x = oot_pt_lin(srgb[c]), d = fabsf(x - k);
        if (!(d < reach)) return 0;   /* this channel is already far enough: the key cannot reach the colour */
        const float to = k >= 0.5f ? k - reach : k + reach, need = fabsf(to - x);
        if (best < 0 || need < best_d) { best = c; best_d = need; best_to = to; }
    }
    srgb[best] = oot_pt_srgb(best_to);
    return 1;
}

/* RuntimeLink message 101 (our streamer change): u8 mode (0 off, 1 RGB chroma key), u8 red, green, blue, u8 threshold (1..255),
   u8 feathering in % (1..100), u16 0. */
static inline void oot_pt_payload(int on, uint8_t p[8]) {
    memset(p, 0, 8);
    p[0] = on ? 1 : 0; p[1] = OOT_PT_KEY_R; p[2] = OOT_PT_KEY_G; p[3] = OOT_PT_KEY_B; p[4] = OOT_PT_THRESHOLD; p[5] = OOT_PT_FEATHER_PCT;
}

/* Once per headset frame. option: the VR page's passthrough; offered: the streamer said it takes message 101 (an older streamer
   does not, and drops the link on a message it does not know); screen: 1 a screen or the window is shown (no head view), 0 the
   head view, -1 no picture this frame (kept as it was). */
typedef struct { int sent; int screen; uint64_t last_key_ns, last_send_ns; } oot_pt_state;   /* sent: -1 nothing yet, 0 off, 1 on */
typedef struct { int fill_key, guard, send, send_on; } oot_pt_step_out;
static inline void oot_pt_init(oot_pt_state *s) { memset(s, 0, sizeof *s); s->sent = -1; }
static inline oot_pt_step_out oot_pt_step(oot_pt_state *s, int option, int offered, int screen, uint64_t now) {
    oot_pt_step_out o = { 0, 0, 0, 0 };
    if (!offered) { s->sent = -1; return o; }   /* a new streamer link starts from ALVR's default (off) again */
    if (screen >= 0) s->screen = screen != 0;
    const int want = option && s->screen;
    if (want) s->last_key_ns = now;
    const int keying = want || (s->sent == 1 && now - s->last_key_ns < OOT_PT_HOLD_NS);
    o.fill_key = want;
    o.guard = keying || s->sent == 1;   /* the Quest may still key: no picture colour within its reach */
    if ((s->sent != keying && !(s->sent == -1 && !keying)) || (keying && now - s->last_send_ns >= OOT_PT_RESEND_NS)) {
        o.send = 1; o.send_on = keying; s->sent = keying; s->last_send_ns = now;
    }
    return o;
}
