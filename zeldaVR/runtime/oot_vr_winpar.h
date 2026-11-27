/* The window view's parallax (vr.md row 3, its second step), without the runtime (tests/test_oot_vr_winpar.c runs it). With the
   window (oot_vr_window.h) the game's own camera is shown in a frame fixed in the room; seen "through" it the world stayed as if far
   away, because the game's camera did not move when the head moved against the frame. Here, as HookshotVR's window view does
   (vr/headset/window_view.inc, ideas and facts only, GPL-3, no code: "no turn (the orbit camera's own axes are the frame's) and the
   head's place from the frame's viewpoint ..., in the frame's axes (x right, y up, z back), metres", "metre for metre"; "the
   headset crops each eye's picture ... as seen from where THAT picture was drawn"; "The game's FOV stays the headset's"):

     1. The VR thread measures the head from the anchor (where the head was when the frame was placed) in the frame's own axes, and
        hands that offset to the game's camera hook instead of a head turn (oot_win_par_offset). The game's camera axes are taken as
        the frame's, so the offset is in the camera's own axes, one game unit a metre (OOT's units are metres: first person puts the eyes
        1.60 m over adult Link's feet, child Link's x 125/160, oot_vr_stature.h oot_eye_height).
     2. The camera hook moves the game's view by it, in the view's own axes, no turn (oot_win_par_view).
     3. Each picture keeps the viewpoint it was drawn from (the anchor plus the offset its camera took; plus the eye shift along the
        frame's x when depth stereo made it an eye's picture), by its head sequence (oot_wpar_put / oot_wpar_get), and the window
        maps it from there (oot_win_through_uv with that viewpoint): a point near the frame lands where a real window would show it
        from that viewpoint; a far point lands where the window without parallax showed it.

   vr_cam_state.off[3] (otherwise 0) says the offset is in the camera's own axes and the hook must do only this: OOT_WIN_PAR_MARK.
   Metres, OpenXR axes: y up, -z forward. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "oot_vr_window.h"

#define OOT_WIN_PAR_MARK 1.0f          /* vr_cam_state.off[3]: the window's parallax (the camera's own axes, no turn) */
static const float OOT_WIN_PAR_MAX_M = 1.0f;   /* the offset at most this far each way, as the head offset (offset_update) */

/* Why the parallax is or is not used (one log line each time this changes). */
enum { OOT_WPAR_ON, OOT_WPAR_NOT_WINDOW, OOT_WPAR_OFF_OPTION, OOT_WPAR_NO_FRAME, OOT_WPAR_NO_FOV, OOT_WPAR_BAD_HEAD, OOT_WPAR_N };
static const char *const oot_wpar_why_text[OOT_WPAR_N] = {
    "on: the game's camera moves with the head against the window, each picture seen through it from where it was drawn",
    "not used: the screens are the curved screen (screen_shape 0)",
    "not used: the VR page's window parallax is off (win_parallax 0); the window shows the game's camera as if far away",
    "not used: the window has not been placed yet",
    "not used: the game does not render the headset's field of view (no Vert+ stubs), so nothing is seen through the window",
    "not used: the head's place is not finite",
};

static inline float oot_wpar_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* 1. The head's offset from the anchor in the frame's axes (x right, y up, z out toward the viewer), each way at most
   OOT_WIN_PAR_MAX_M. 0 when a place is not finite. */
static inline int oot_win_par_offset(const oot_win_frame *f, const float anchor[3], const float head[3], float off[3]) {
    float a[3], h[3];
    oot_win_local(f, anchor, a); oot_win_local(f, head, h);
    for (int i = 0; i < 3; i++) { const float d = h[i] - a[i]; if (!isfinite(d)) return 0; off[i] = oot_wpar_clampf(d, -OOT_WIN_PAR_MAX_M, OOT_WIN_PAR_MAX_M); }
    return 1;
}
/* The viewpoint the camera then draws from, in the frame's axes: the anchor's place plus the offset. */
static inline void oot_win_par_from(const oot_win_frame *f, const float anchor[3], const float off[3], float from[3]) {
    oot_win_local(f, anchor, from);
    for (int i = 0; i < 3; i++) from[i] += off[i];
}

/* 2. The game's view matrix m (3 x 4, world to camera, rows the camera's right, up and back in the world, column 3 the translation)
   moved by off in its own axes: the camera's place goes by +off, so each camera coordinate drops by off (t' = t - off). No turn.
   0 (m unchanged) when anything is not finite or the offset is beyond OOT_WIN_PAR_MAX_M (not ours). */
static inline int oot_win_par_view(float m[12], const float off[3]) {
    for (int i = 0; i < 12; i++) if (!isfinite(m[i])) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(off[i]) || fabsf(off[i]) > OOT_WIN_PAR_MAX_M * 1.001f) return 0;
    for (int i = 0; i < 3; i++) m[4 * i + 3] -= off[i];
    return 1;
}

/* 3. The viewpoints by head sequence (the VR thread writes and reads them; 64 as the head poses g.hl_pose). */
typedef struct { uint32_t seq[64]; float from[64][3]; } oot_wpar_ring;
static inline void oot_wpar_put(oot_wpar_ring *r, uint32_t seq, const float from[3]) {
    if (!seq) return;
    memcpy(r->from[seq % 64], from, sizeof r->from[0]); r->seq[seq % 64] = seq;
}
/* 1: the picture of this sequence was drawn by the window's parallax, from `from` (the frame's axes). */
static inline int oot_wpar_get(const oot_wpar_ring *r, uint32_t seq, float from[3]) {
    if (!seq || r->seq[seq % 64] != seq) return 0;
    memcpy(from, r->from[seq % 64], sizeof r->from[0]);
    return 1;
}
/* An eye's picture of depth stereo (vr_ds_eyes): made as if the camera were `shift` further along its own x (the frame's x),
   the left eye -shift; pic_eye < 0 or shift <= 0: the one picture. */
static inline void oot_wpar_eye(float from[3], int pic_eye, float shift) {
    if (pic_eye >= 0 && shift > 0 && isfinite(shift)) from[0] += pic_eye ? shift : -shift;
}
