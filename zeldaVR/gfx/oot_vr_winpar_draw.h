/* zeldaVR, Ocarina of Time: the window view's parallax (vr.md row 3, its second step; the math in zeldaVR/runtime/oot_vr_winpar.h,
   the game's side in games/oot/runtime/oot_vr_winpar_cam.h). Included by totk_vr.m after oot_vr_window_draw.h (GAME_OOT); the VR
   thread only.

   Option win_parallax (the VR page, off by default; with it off nothing here runs and the window is as committed in a21e531c).
   Used while the window (screen_shape 1) shows the game's own camera: the big screen of the left-stick shift + Menu, or a scene shown
   on a screen in first person, the two cases headlook_update turned the head view off for. Then, each headset frame:
     - oot_win_par_head (headlook_update, g_camera_control_mu held): the control on with no turn (h the identity), no levelling, and
       the head's offset from the window's anchor in the frame's axes (off[3] = OOT_WIN_PAR_MARK; offset_update leaves it alone while
       g_oot_win_par_on); the head pose kept in g.hl_pose like the head view's, and the viewpoint the camera will draw from (the
       anchor plus the offset, the frame's axes) kept by the same sequence in g_wpar.
     - oot_window_posed (each eye's draw): a picture whose sequence is one of those is seen through the frame from its own viewpoint
       (plus half the eye distance along the frame's x for depth stereo's eye pictures, as vr_ds_eyes warped it), not from the eye
       now: HookshotVR's crop "from where that picture was drawn". Its frame then holds in the room and the parallax lags only by the
       game's latency. Without a HUD split (the world and the HUD in one picture) it is seen through the window too, the HUD inside it.
   One log line each time the reason it is used or not changes ([vr-winpar]). */
#include "../runtime/oot_vr_winpar.h"

extern int runtime_oot_vr_fp_active(void) __attribute__((weak_import));
static oot_wpar_ring g_wpar;   /* the viewpoints by head sequence */
static int g_wpar_why = -1;
static void oot_wpar_say(int why) {
    if (why == g_wpar_why || why < 0 || why >= OOT_WPAR_N) return;
    g_wpar_why = why; VLOG("[vr-winpar] %s", oot_wpar_why_text[why]);
}
/* headlook_update, where the head view would be off (the big screen, a scene on a screen). 1: the control is the parallax's. */
static int oot_win_par_head(vr_cam_state *s, const vr_pose *head) {
    int why = OOT_WPAR_ON;
    if (vr_oot_screen_shape() != OOT_WIN_FRAME) why = OOT_WPAR_NOT_WINDOW;
    else if (!vr_oot_win_parallax()) why = OOT_WPAR_OFF_OPTION;
    else if (!g_win_have) why = OOT_WPAR_NO_FRAME;
    else if (!(g.fov_on > 0)) why = OOT_WPAR_NO_FOV;
    float off[3] = { 0, 0, 0 }, from[3] = { 0, 0, 0 };
    if (why == OOT_WPAR_ON) {
        const oot_win_opts o = oot_window_opts();
        const oot_win_shape sh = oot_win_shape_of(&o);
        const oot_win_frame f = oot_win_place(g_win_at, g_win_yaw, &sh);
        if (oot_win_par_offset(&f, g_win_at, head->p, off)) oot_win_par_from(&f, g_win_at, off, from);
        else why = OOT_WPAR_BAD_HEAD;
    }
    oot_wpar_say(why);
    g_oot_win_par_on = why == OOT_WPAR_ON;
    if (!g_oot_win_par_on) return 0;
    const uint32_t seq = s->seq + 1;
    static const float id[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };   /* rows of the identity: no head turn */
    g.hl_pose[seq % 64] = *head;
    memcpy(g.hl_h[seq % 64], id, sizeof id); g.hl_hseq[seq % 64] = seq;
    memcpy(s->h, id, sizeof id);
    oot_wpar_put(&g_wpar, seq, from);
    __atomic_store_n(&s->level, 0u, __ATOMIC_RELAXED);
    const float o4[4] = { off[0], off[1], off[2], OOT_WIN_PAR_MARK }; memcpy(s->off, o4, sizeof o4);
    g.hl_have_sent = 0;   /* back in the head view, its first turn is sent whole (the dead band compares with the last head turn sent) */
    __atomic_store_n(&s->seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&s->on, 1, __ATOMIC_RELEASE);
    return 1;
}
/* The head view again (headlook_update on its own path): the parallax lets go of the control. */
static void oot_win_par_release(void) {
    if (g_oot_win_par_on) { g_oot_win_par_on = 0; VLOG("[vr-winpar] the head view again: the window's parallax lets the camera go"); g_wpar_why = -1; }
}
/* One eye's picture, the window's way. 0: not the window's (the caller draws it as before); 1: done.
   used: the head sequence the picture was drawn with (0 none); pic_eye, half: depth stereo's eye picture and half its eye distance. */
static int oot_window_posed(id<MTLRenderCommandEncoder> e, const float m[16], id<MTLTexture> pic, const float eye[3], int world,
                            uint32_t used, int pic_eye, float half) {
    if (!used) return oot_window_draw(e, m, pic, eye, world);
    float from[3];
    if (!oot_wpar_get(&g_wpar, used, from)) return 0;   /* a head view's picture */
    /* depth stereo moved the eye by half * the world's scale in first person (totk_vr.m render_frame, vr_ds_eyes' warp) */
    const float k = runtime_oot_vr_fp_active && runtime_oot_vr_fp_active() ? vr_oot_world_scale() : 1;
    oot_wpar_eye(from, pic_eye, half * k);
    /* A picture of the moved camera is drawn with the headset's field of view whether or not its HUD was split off: it is seen
       through the window either way, the HUD then inside it (the user's choice A, 2026-11-27). Fitting it whole when the HUD was
       not split made the window jump between the two looks (batch18: "seen through" and "fitted" took turns). */
    { static int said = -1; const int now = world ? 1 : 0;
      if (now != said) { said = now;
          VLOG("[vr-winpar] pictures seen through the window from where each was drawn (this one from %.3f %.3f %.3f m, the frame's axes%s; %s)",
               (double)from[0], (double)from[1], (double)from[2], pic_eye >= 0 && half > 0 ? ", an eye of depth stereo" : "",
               now ? "its HUD split off" : "its HUD inside the picture"); } }
    return oot_window_draw_from(e, m, pic, eye, 1, from);
}
