// TOTK: MetalFX temporal anti-aliasing (TOTK_TAA=1; not in D2R). See totk_taa.m.
#pragma once
#import <Metal/Metal.h>

int taa_enabled(void);                         // on in the settings panel (what it shows)
int taa_effective(void);                       // a frame has been anti-aliased
int taa_active(void);                          // the replay's state, latched from it at frame end
int taa_ready(void);                           // this frame has the depth and camera to anti-alias (cheap check before taa_encode)
void taa_set(int on);                          // the settings panel (key 1, runtime/frontend.m)
/* Per render pass (before its encoder opens): finds this frame's G-buffer pass (D32F depth of the colour size, three or
   more colour attachments) and returns 1 for the passes that draw into its depth, which get this frame's jitter. */
int taa_pass(MTL4RenderPassDescriptor *pd);
int taa_jittered(void);                        // the current pass draws with the jitter
void taa_jitter(double *x, double *y);         // this frame's jitter in pixels (viewport origin offset)
void taa_note_camera(const float *cam, int n); // the G-buffer pass camera block (see mfx_take_camera)
/* At the game's compose draw (0ac9a549, tone mapping of the linear HDR scene): the anti-aliased scene of the same size,
   or nil (no depth or camera this frame, scaler not available). Encodes into cb outside any encoder. */
id<MTLTexture> taa_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> scene);
id<MTLResidencySet> taa_residency(void);       // our textures and buffers; the queue adds it once
void taa_frame_end(void);                      // at the game's present
void taa_stats(char *out, size_t n);
/* MetalFX temporal upscaling (TOTK_MFX_TEMPORAL=1 / flag mfx_temporal; experimental, off by default): the scene size to the
   canvas at the game's own upscale draw, with key 5's jitter, depth, camera motion and mask; while the game draws an
   upscale it can take, it is the frame's one temporal pass (key 5's is not run). */
int tup_enabled(void);
void tup_set(int on);
float tup_mip_bias(void);                      // TOTK_MFX_TEMPORAL_MIPBIAS for the material samplers (0: none)
/* At the upscale draw: 1 when this frame can be upscaled from color (cw x ch at its origin, the scene's pixel grid) into
   dst (the canvas); 0 keeps the caller's own path (a scaler still being built in the background, no depth or camera). */
int tup_ready(id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst);
/* After tup_ready, outside any encoder: motion vectors, the scaler and the copy into dst (all of dst); mark is called
   before each ('T', 'X', 'Y': TOTK_PASS_GPU entries). 1 when dst holds the upscaled picture. */
int tup_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> color, NSUInteger cw, NSUInteger ch, id<MTLTexture> dst, void (^mark)(char kind));
void tup_stats(char *out, size_t n);
void tup_log_frames(long n);                   // flag tup_log: a line per upscaled frame (jitter, scale, reset, camera motion)
void tup_note_site(void);                      // the replay found an upscale draw tup could take this frame
int tup_active(void);                          // latched at frame end: this frame's temporal pass is the upscale
