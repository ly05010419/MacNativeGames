// TOTK: MetalFX frame interpolation (not in D2R). See totk_mfx.m.
#pragma once
#import <Metal/Metal.h>

/* Odyssey supports off / 60+60 only, at a nominal 120 Hz window-screen rate.
   Key 2 toggles the request; nonzero legacy modes map to the same 60+60 request.
   The request survives unsupported screen rates, while generation is suspended.
   mfx_latch applies the effective mode at a game-frame boundary. */
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
// between the previous and this frame, or -1 (first frame, missing input, scene cut; state already advanced). frame_s:
// the pacing period (two display refreshes; 0 in unpaced measurement runs). encode: motion vectors and the interpolation into cb (Metal 4, own command buffer, GPU order after this
// frame). abort: a prepared frame that cannot be encoded (no free command buffer); drops the history.
int mfx_frame_prepare(double frame_s);
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
