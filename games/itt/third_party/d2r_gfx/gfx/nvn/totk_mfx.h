// TOTK: MetalFX frame interpolation (not in D2R). See totk_mfx.m.
#pragma once
#import <Metal/Metal.h>

/* Frame interpolation modes (user, 2026-11-03: key 2 cycles off -> half -> full -> off, on top of the key 8 frame rate):
   MFX_HALF one generated frame for every second game frame ("60+30" at 60 fps);
   MFX_FULL one generated frame for every game frame ("60+60" at 60 fps).
   The game keeps the key 8 pacing in every mode; screen refresh limits presentation, not generation.
   TOTK_MFX=half | full (old names: interp = half, interp30 / interpmax = full) starts in that mode; flag mfx_interp
   <0..2> sets it, -1 (key 2) cycles. A change takes effect at the next game frame boundary (mfx_latch at the present),
   so a frame is never half on. */
#include "totk_mfx_mode.h"
const char *mfx_mode_name(int mode);
void mfx_set(int mode);  // mode < 0 cycles the requested mode
int mfx_latch(void);     // at the game's present, after this frame's interpolation: take the requested mode; 1 = it changed
// Per game frame, from the replay: the scene copy (the canvas copied before the first UI draw, in the canvas format),
// the UI layer (colour attachment 7 of the later canvas passes), the G-buffer depth and the camera block (84 floats of
// the 2368-byte block at vertex uniform 1).
id<MTLTexture> mfx_scene_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h, MTLPixelFormat format);
id<MTLTexture> mfx_ui_target(id<MTLDevice> dev, NSUInteger w, NSUInteger h);
void mfx_note_scene(void);
void mfx_note_depth(id<MTLTexture> depth);
void mfx_note_camera(const float *cam, int n);
id<MTLResidencySet> mfx_residency(void);   // our textures; the queue adds it once
// At the game's present. prepare: checks this frame's inputs and returns the output slot of the interpolated frame
// between the previous and this frame, or -1 (first frame, missing input, scene cut, half mode's other frame).
// frame_s is the game pacing period; encode submits motion and interpolation after the real game frame.
int mfx_frame_prepare(double frame_s);
int mfx_frame_publish(void); // half mode still encodes hidden pairs to maintain MetalFX history
/* Ports without the game-specific scene / UI / camera hooks (It Takes Two, Diablo III, Minecraft Dungeons 1 / 2): the
   whole presented picture is interpolated. Per game frame, into cb: the picture is copied (decoded to linear light), and
   when a generated frame is due the interpolation runs with a constant depth, zero motion (MetalFX finds the motion
   itself) and an empty UI layer. Returns the resources to keep until cb completes (nil: nothing encoded) and sets *slot
   to the output slot or -1. Unsupported picture formats are left alone (logged once). */
NSArray *mfx_generic_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, double frame_s,
                            id<MTL4CounterHeap> heap, NSUInteger stamp, int *slot);
// Keep the returned resources alive until this command buffer's GPU completion.
// Cached interpolators drop their public texture bindings after encoding.
NSArray *mfx_frame_encode(id<MTL4CommandBuffer> cb, int slot, id<MTL4CounterHeap> heap, NSUInteger stamp);   // heap: timestamps at stamp / stamp+1 around MetalFX (may be nil)
void mfx_note_gpu(double ms);   // GPU time of one interpolation, for the statistics line
void mfx_frame_abort(void);
id<MTLTexture> mfx_output(int slot);   // RGBA8Unorm view of the sRGB output, like the swapchain textures
void mfx_stats(char *out, size_t n);
void mfx_mv_log(void);
int mfx_dump_due(void);
void mfx_dump(long frame, id<MTLTexture> swap_prev, id<MTLTexture> swap_cur);   // TOTK_MFX_DUMP=<dir>: scenes, depth, motion and output of three frames while the camera turns (diagnostic)   // TOTK_MFX_MVLOG=<n>: motion texture median every n interpolated frames (diagnostic)
