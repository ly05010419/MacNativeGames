// TOTK: MetalFX spatial upscaling (TOTK_MFX_UPSCALE; not in D2R). See totk_mfx_up.m.
#pragma once
#import <Metal/Metal.h>

/* One scaler context per use; each context is used from one thread only. */
enum {
    MFXUP_SCENE = 0,    // iOS, handheld: the scene below the canvas size (TOTK_RENDER_RES) before the compose shader samples it
    MFXUP_DOCKED = 1,   // Mac, docked: the game's own upscale of the finished picture into the 1080p canvas, replaced
    MFXUP_OUTPUT = 2,   // Mac, the present: the finished 1080p picture into a bigger window / full screen (2K output), used from the present thread
    MFXUP_NCTX
};
/* Colour processing modes: the values of MTLFXSpatialScalerColorProcessingMode, without importing MetalFX here. */
enum { MFXUP_PERCEPTUAL = 0, MFXUP_LINEAR = 1, MFXUP_HDR = 2 };

int mfxup_enabled(void);   // TOTK_MFX_UPSCALE: default off; key 3 enables it in the manual MetalFX picture mode
int mfxup_output_enabled(void);   // TOTK_MFX_OUTPUT: default off; key 3 enables the window output stage in the manual MetalFX picture mode
void mfxup_output_set(int on);    // runtime: "flag mfx_output 0|1" (the output stage alone, for A/B)
void mfxup_set(int on);    // runtime: "flag mfx_upscale 0|1"
/* Encodes a MetalFX spatial upscale of the top-left in_w x in_h region of src into cb (Metal 4; the caller has ended its
   render / compute encoders). dst nil: into the context's own out_w x out_h texture; otherwise into dst itself (its format
   and usage must suit MetalFX; out_w / out_h are dst's size). mode: an MTLFXSpatialScalerColorProcessingMode. Returns the
   texture written, or nil when this configuration cannot be built or is still being built in the background (the caller
   keeps its own path for this frame). */
id<MTLTexture> mfxup_encode_ex(int ctx, id<MTL4CommandBuffer> cb, id<MTLTexture> src, NSUInteger in_w, NSUInteger in_h,
                               id<MTLTexture> dst, NSUInteger out_w, NSUInteger out_h, int mode);
/* Whether the scaler for this configuration is ready (1); otherwise it is being built on a background queue (about 70 ms) and
   the caller keeps its own path for now. A caller that must end its encoders before encoding asks first. */
int mfxup_prepare(int ctx, id<MTLTexture> src, id<MTLTexture> dst, NSUInteger out_w, NSUInteger out_h, int mode);
// The iOS hook's form: all of src into the scene context's own texture, HDR colour processing (the scene before tone mapping).
id<MTLTexture> mfxup_encode(id<MTL4CommandBuffer> cb, id<MTLTexture> src, NSUInteger out_w, NSUInteger out_h);
id<MTLResidencySet> mfxup_residency(void);   // the contexts' own output textures (nil until one exists); the queue adds it once
void mfxup_stats(int ctx, char *out, size_t n);
uint64_t mfxup_switches(int ctx);   // times the current scaler changed to another cached one (logged once per scaler)
