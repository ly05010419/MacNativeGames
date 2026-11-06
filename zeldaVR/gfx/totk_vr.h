// TOTK VR (Mac, TOTK_VR_CONTROL=<control file of `basalt-streamer --wired --runtime <file>`>): the game's picture on a
// large screen fixed in the room, seen through a Quest. Stage 1 of the VR plan (vr研究.md): the game renders as usual;
// at each present we copy its picture, and a thread of our own renders both eyes at the headset's rate with the newest
// head pose and hands them to the BasaltVR streamer (totk_vr_link.c). See totk_vr.m.
#pragma once
#import <Metal/Metal.h>

int vr_enabled(void);
/* Present thread, a game frame (not an interpolated one), after the window's present is encoded into cb: copies the
   picture (crop: x, y, w, h inside src, or NULL / zero size for all of it) for the headset. Returns the copy's slot, to
   pass to vr_source_done from the command buffer's feedback handler, or -1. */
int vr_source_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const int crop[4], uint64_t ticket);
int vr_source_encode_eye(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const int crop[4], int eye);   /* stereo B2: the left pass's picture */
void vr_b2_present_eye(int eye);
void vr_b2_pair_begin(void);
void vr_b2_eye_begin(void);   /* stereo B2: the next present's picture is that eye's */
/* Replay thread: the projection belongs to the actual image being captured. */
void vr_source_projection(const float fov[4],const float rotation[9]);
void vr_packet_trace(int packets);
void vr_source_done(int slot, int ok);
id<MTLResidencySet> vr_residency(void);   // our copies of the picture; the queue adds it once
void vr_recenter(void);
void vr_set_deadband(int tenths);
int vr_first_person(void);                 // head look: the camera at the player's eyes (1) or behind him (0)
void vr_set_first_person(int on);
void vr_set_eye_cm(int cm);         // head look: rotations under this many tenths of a degree leave the game camera still
void vr_set_lean_cm(int cm);        // first person: looking down moves the eyes up to cm forward
void vr_set_eye_bone(int on);       // first person: 1 the eyes on the head bone (experimental), 0 the fixed height (default)
void vr_set_pitch_lock(int on);     // 1: up / down only from the headset (the game's pitch out, the stick's up / down off in the world)
int vr_offset_cm(int axis);         // the camera offset in cm: 0 up, 1 forward, 2 right (the VR page's sliders)
void vr_set_offset_cm(int axis, int cm);
int vr_wants_camera(void);                /* stage 2 head look is on: the replay hands each frame's camera to vr_note_camera */
void vr_note_camera(const float *cam);    /* the frame's main camera block (view matrix first, 3x4 rows): finds its head pose */
void vr_present_queued(uint64_t ticket);  /* the game presents that frame: its head pose goes with the present's ticket */                   // put the screen in front of where the head looks now
/* Native same-frame SBS, captured at the game's lens compositor after the HUD.
 * Copy both eyes into independent array slices; publish once at GPU completion. */
int vr_source_encode_stereo(id<MTL4CommandBuffer> cb, id<MTLTexture> src, const float fov[2][4], const float rotation[2][9]);
int vr_source_encode_stereo_menu(id<MTL4CommandBuffer> cb, id<MTLTexture> src, float aspect);
void vr_source_native_frame(void); /* This game's frame has the native lens consumer. */
void vr_stats(char *out, size_t n);
/* BotW160: eight render sizes apply at the next game frame; streaming stays 2048x2208
   per eye. Other adapters retain their existing streaming-size setting. */
int vr_streamer_managed(void);
void vr_resolution(int *w, int *h);
void vr_stream_resolution(int *w,int *h);
int vr_set_resolution(int w, int h);
int vr_frame_pos(float p[3]);   /* the replay thread: the position this frame's first-person camera was put at (0 without one) */
void vr_frame_match_desc(char *out, size_t n);   /* diagnostic: this frame's camera block against the hook ring, then reset */
int vr_frame_ring(void);
/* Stereo by alternating eyes (the VR page's "立体（左右眼轮流渲染）", option afr, off by default; flag vr_afr): the game
   renders its frames for the left and the right eye in turn and each eye sees its own newest picture. */
int vr_afr(void);
void vr_set_afr(int on);
int vr_ipd_pct(void);            /* its eye distance, percent of the headset's IPD (30..200; flag vr_ipd_pct) */
void vr_set_ipd_pct(int pct);
