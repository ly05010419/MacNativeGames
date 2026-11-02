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
void vr_stats(char *out, size_t n);
/* VR settings (TOTK_VR_STREAMER: the game runs the streamer): the per-eye size streamed, and a new one (restarts the
   streamer; the VR thread reconnects). */
int vr_streamer_managed(void);
void vr_resolution(int *w, int *h);
int vr_set_resolution(int w, int h);
int vr_frame_pos(float p[3]);   /* the replay thread: the position this frame's first-person camera was put at (0 without one) */
void vr_frame_match_desc(char *out, size_t n);   /* diagnostic: this frame's camera block against the hook ring, then reset */
int vr_frame_ring(void);         /* the replay thread: the ring entry (vr_cam_state.ring) of this frame's camera, -1 without one */
