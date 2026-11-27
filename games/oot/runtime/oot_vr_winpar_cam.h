/* The window view's parallax, the game's side (vr.md row 3; the math in zeldaVR/runtime/oot_vr_winpar.h, the VR side in
   zeldaVR/gfx/oot_vr_winpar_draw.h). Included by oot_vr.h before oot_vr_camera, which calls oot_win_par_camera right after it found
   the control on (the game's thread, the gameplay view, its matrix just written at view +8: the veneer at main+0x4c5068 as before;
   no new hook site).

   While the window shows the game's own camera (the big screen of the left-stick shift + Menu, or a scene shown on a screen in first
   person) and the VR page's window parallax is on, the VR thread turns the control on with no head turn and the head's offset from
   the window's anchor in the camera's own axes, off[3] = OOT_WIN_PAR_MARK. Here only that: the view moved by the offset, nothing else
   the head view does (no levelling, no first person, no scene seat, no third-person distance, no stick turn). The ring entry the
   replay matches each frame's camera block against gets this frame's head sequence (so its picture carries the viewpoint it was drawn
   from), at an index of its own: the camera hook's call count (vr_cam_state.calls) is not bumped, so the controllers still see no
   world camera running (totk_vr.m world_camera_running: gestures, the stick's turn and pitch lock stay as on the big screen now). */
#pragma once
#include "../zeldaVR/runtime/oot_vr_winpar.h"

static int oot_wpar_cam_on;   /* the game's thread: the parallax had the last gameplay view (for the log) */
/* 1: the parallax had the view (moved, or left as the game's when a value was not as expected); 0: not the parallax's control. */
static int oot_win_par_camera(uint8_t *view, const vr_cam_state *control) {
    if (control->off[3] != OOT_WIN_PAR_MARK) {
        if (oot_wpar_cam_on) { oot_wpar_cam_on = 0; fprintf(stderr, "[vr-winpar] the game's camera no longer moves with the head against the window\n"); }
        return 0;
    }
    pthread_mutex_lock(&oot_vr_mu); oot_vr_base_valid = 0; pthread_mutex_unlock(&oot_vr_mu);
    float m[12]; memcpy(m, view + 8, sizeof m);
    if (!oot_win_par_view(m, control->off)) {
        static int said; if (said < 5) { said++; fprintf(stderr, "[vr-winpar] 【红】the view or the offset is not finite (or beyond %.0f m): the game's own view this frame\n", (double)OOT_WIN_PAR_MAX_M); }
        return 1;
    }
    memcpy(view + 8, m, sizeof m);
    static uint32_t n;
    const uint32_t i = ++n & 15;
    memcpy(oot_vr_cam.ring[i].m, m, sizeof m);
    oot_vr_cam.ring[i].seq = control->seq;
    memset(oot_vr_cam.ring[i].pos, 0, sizeof oot_vr_cam.ring[i].pos);
    __atomic_store_n(&oot_vr_cam.used, control->seq, __ATOMIC_RELEASE);
    __atomic_store_n(&oot_vr_cam.cam, (uint64_t)(uintptr_t)view, __ATOMIC_RELEASE);
    if (!oot_wpar_cam_on) { oot_wpar_cam_on = 1;
        fprintf(stderr, "[vr-winpar] the game's camera moves with the head against the window (no turn; now %.3f %.3f %.3f m along its right, up, back; head sequence %u)\n",
                (double)control->off[0], (double)control->off[1], (double)control->off[2], control->seq); }
    return 1;
}
