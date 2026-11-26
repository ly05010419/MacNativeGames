// Depth-reprojected stereo for a head-look game that renders one picture from between the eyes (Ocarina of Time,
// zeldaVR; user 2026-11-22: "转头时才显得拉伸，那就是缺立体", stereo by depth reprojection). Each new game picture is
// turned into a left-eye and a right-eye picture from the scene depth of the same frame; the VR thread then places each
// eye's picture where that eye was when the frame was rendered (totk_vr.m place_picture, half the eye distance).
//
// Threads: the replay thread notes the frame's scene depth (vr_ds_note_scene) and, at the game's present, copies it
// into a ring slot of ours (vr_ds_capture_target, then vr_ds_captured); the ticket of that present carries the slot
// (vr_ds_take_captured) to the picture copy; the VR thread warps (vr_ds_eyes). See vr_depth_stereo.m.
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>
#include <stddef.h>

/* Replay thread: this frame's scene depth (Depth32Float, as large as the camera's view: cam_w x cam_h) and its camera
   block's near plane (block float 56). Depth not reversed, far plane at infinity: distance = near / (1 - d). */
void vr_ds_note_scene(id<MTLTexture> depth, float near_plane, float cam_w, float cam_h);
/* Replay thread, at the game's present: the ring texture to copy this frame's depth into (and *from, the depth noted),
   or nil (stereo off, nothing noted, VR not started). After encoding the copy, vr_ds_captured(). */
id<MTLTexture> vr_ds_capture_target(id<MTLTexture> *from);
void vr_ds_captured(void);
/* Replay thread, vr_present_queued: the slot just captured, as a reference for the ticket (0: none). */
uint32_t vr_ds_take_captured(void);
/* VR thread: the left and right eye's pictures for the picture of sequence seq with depth reference
   dref; encoded into cb once per seq. tan_y / aspect: the vertical half-angle tangent and the width over height of the
   3D view the game rendered (it fills the picture's height, centred); half: half the eye distance (game units = m).
   1 with out[0] / out[1] set, 0 when the picture stays one for both eyes (the reason is counted). */
int vr_ds_eyes(id<MTLCommandBuffer> cb, id<MTLTexture> pic, uint64_t seq, uint32_t dref, float tan_y, float aspect,
               float half, __strong id<MTLTexture> out[2]);
/* The cap on a near thing's shift, in pixels of a 1080-row picture: 8..64, 64 by default (the VR page's "near depth"). */
void vr_ds_set_max_shift(int px);
/* The HUD on a plane of its own (Ocarina of Time): the replay thread copies the canvas just before the frame's first UI draw
   (vr_ui_scene_target, vr_ui_scene_captured), the present's ticket carries the copy (vr_ui_take_scene). The VR thread then
   parts the finished picture into the scene without the HUD and the HUD with a coverage in alpha: what differs from the
   copy is HUD. 1 with both set (they stay valid until the next call); 0: no copy for this picture. */
id<MTLTexture> vr_ui_scene_target(id<MTLTexture> canvas);
void vr_ui_scene_captured(void);
uint32_t vr_ui_take_scene(void);
int vr_ui_split(id<MTLCommandBuffer> cb, id<MTLTexture> pic, uint64_t seq, uint32_t ref, __strong id<MTLTexture> *scene, __strong id<MTLTexture> *ui);
/* Statistics for the VR line (and reset): pictures warped, kept mono and why. */
void vr_ds_stats(char *out, size_t n);
