// FXAA off and sharpening (Mac, Breath of the Wild 1.9.0; the effects page's rows, as 1.4.3's totk_finish.h).
// Game-specific and never needed to run the game; included by nvn_mtl.m right after totk_finish.h, on GAME_BOTW190 only.
// The game's FXAA bd7ffdd4 (FXAA 3.11, as TOTK's 22a61cb1) is also its upscale: the one draw reads the finished picture
// and writes the canvas (RGB only, mask 0x7), and the UI is drawn into the canvas after it. With MetalFX upscaling on and
// a picture smaller than the canvas, mfxup_dock replaces the whole draw (MetalFX anti-aliases instead); at the native
// sizes the picture is the canvas's size and the game's FXAA runs. So, unlike TOTK's, both act around that one draw:
// - FXAA off: the draw is not encoded; our pass draws its source into the canvas instead, texel for texel when the sizes
//   are equal, bilinear when they differ, RGB only (the alpha stays, as the game's mask leaves it; a clear still pending
//   for the canvas is done by the pass). Only a plain full-canvas draw is taken (one colour target, no blending, RGB
//   written and alpha not, the whole viewport and scissor); anything else keeps the game's own and is logged once.
// - Sharpening: FSR 1's RCAS (totk_finish.h fin_pass) at 25 to 100 % on the canvas, once per frame, after the scene is
//   in it (the draw above, ours, or MetalFX) and before the next draw that writes or reads the canvas: the first UI draw,
//   or in a frame without UI the copy to the screen. So the UI is not sharpened, and key 2's scene copy, taken at that
//   same UI draw, is the sharpened scene. Through a scratch image of the canvas's size, copied back.
// Both read totk_finish.h's switches (dm_gfx_fxaa_*, dm_gfx_sharpen_*; "flag fxaa_off", "flag sharpen_pct").
static struct {
    int pending; uint64_t frame, canvas_ser, replaced_frame;
    id<MTLTexture> canvas, scratch;
    id<MTLRenderPipelineState> ps[4]; MTLPixelFormat ps_fmt[4]; int ps_scale[4];
    id<MTL4ArgumentTable> args[3]; int slot;
    uint64_t off_n, sharp_n, declined;
} g_bfin;

/* The game's draw writes RGB and not alpha (mask 0x7, the first target's four bits): our pass writes RGB only, so only
   such a draw is replaced; one that also writes alpha (0xf) keeps the game's own. */
static inline int bfin_mask_rgb_only(uint32_t mask) { return (mask & 0xf) == 0x7; }
/* The FXAA-off pass into format f: texel for texel (scale 0) or bilinear (scale 1), RGB written only. */
static id<MTLRenderPipelineState> bfin_ps(MTLPixelFormat f, int scale) {
    for (int i = 0; i < 4; i++) if (g_bfin.ps[i] && g_bfin.ps_fmt[i] == f && g_bfin.ps_scale[i] == scale) return g_bfin.ps[i];
    int slot = -1; for (int i = 0; i < 4 && slot < 0; i++) if (!g_bfin.ps[i]) slot = i;
    if (slot < 0) return nil;
    static const char src[] =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V bfin_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
        "fragment float4 bfin_copy(V in [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) { return t.read(uint2(in.p.xy)); }\n"
        "fragment float4 bfin_scale(V in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
        "  constexpr sampler s(filter::linear, address::clamp_to_edge); return t.sample(s, in.uv); }\n";
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&e];
    if (!lib) { RED_ONCE(125, "BotW FXAA 关的通道编译失败：%s", e.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"bfin_vs"]; pd.fragmentFunction = [lib newFunctionWithName:scale ? @"bfin_scale" : @"bfin_copy"];
    pd.colorAttachments[0].pixelFormat = f;
    pd.colorAttachments[0].writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { RED_ONCE(126, "BotW FXAA 关的管线建不出（格式 %lu）：%s", (unsigned long)f, e.localizedDescription.UTF8String); return nil; }
    g_bfin.ps[slot] = ps; g_bfin.ps_fmt[slot] = f; g_bfin.ps_scale[slot] = scale;
    return ps;
}
/* The game's FXAA replaced: src into the canvas (its whole area), RGB only; clear: the canvas's pending clear, done first. */
static int bfin_pass(id<MTLTexture> src, id<MTLTexture> dst, int clear, MTLClearColor cc) {
    int scale = src.width != dst.width || src.height != dst.height;
    id<MTLRenderPipelineState> ps = bfin_ps(dst.pixelFormat, scale); if (!ps) return 0;
    int s = g_bfin.slot = (g_bfin.slot + 1) % 3;
    if (!g_bfin.args[s]) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; NSError *e = nil;
                           if (!(g_bfin.args[s] = [g_dev newArgumentTableWithDescriptor:ad error:&e])) return 0; }
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad; rp.colorAttachments[0].clearColor = cc;
    id<MTL4RenderCommandEncoder> re = [g_cb renderCommandEncoderWithDescriptor:rp];
    [re barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];
    [g_bfin.args[s] setTexture:src.gpuResourceID atIndex:0];
    [re setArgumentTable:g_bfin.args[s] atStages:MTLRenderStageFragment];
    [re setViewport:(MTLViewport){0, 0, (double)dst.width, (double)dst.height, 0, 1}];
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    return 1;
}
/* At the FXAA / upscale draw (mfxup_draw): notes the canvas for the sharpening and, with FXAA off and the draw not
   replaced by MetalFX (replaced 0), draws our pass instead. Returns 1 when the draw is not to be encoded. */
static int botw_fin_upscale(__unsafe_unretained NvShader *fs, int replaced) {
    if (strncmp(fs->key, BOTW_MFXUP_KEY, 65)) return 0;
    uint64_t frame = atomic_load(&g_cur_frame);
    int off = atomic_load_explicit(&g_fin_fxaa_off, memory_order_relaxed), pct = atomic_load_explicit(&g_fin_sharpen, memory_order_relaxed);
    if (replaced) g_bfin.replaced_frame = frame;
    g_bfin.pending = 0; g_bfin.canvas = nil;
    if (!pct && (replaced || !off)) return 0;
    const view_t *cv = &R.rt.cview[0]; int hv = R.rt.has_cview[0];
    id<MTLTexture> dst = R.rt.n == 1 && R.rt.color[0] ? attachment_texture(R.rt.color[0], cv, hv) : nil;
    if (!dst || dst.textureType != MTLTextureType2D) return 0;
    if (pct) { g_bfin.pending = 1; g_bfin.frame = frame; g_bfin.canvas_ser = R.rt.color[0]->serial; g_bfin.canvas = dst; }
    if (replaced || !off) return 0;
    int tid = (int)(S.tex[1][0] & 0xfffff);
    id<MTLTexture> src = tid > 0 ? (__bridge id<MTLTexture>)tex_fast_lk(tid) : nil;
    int ok = src && src.textureType == MTLTextureType2D && src != dst && !(S.color.blend_en & 1) && bfin_mask_rgb_only(S.chmask.mask) &&
             !(hv && ((cv->set_levels && cv->base_level) || (cv->set_layers && cv->min_layer))) &&
             !(S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)dst.width && S.vp[0][3] == (float)dst.height)) &&
             scissor_full(R.rt.color[0], cv, hv);
    if (!ok) { if (!g_bfin.declined++) MLOG("[fxaa] BotW: the FXAA draw is not a plain full-canvas draw (source %lux%lu -> canvas %lux%lu, blend %#x, mask %#x): the game's own kept",
                                         (unsigned long)src.width, (unsigned long)src.height, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned)S.color.blend_en, S.chmask.mask);
               return 0; }
    rp_end(); blit_close(); rs_sync();
    pt_mark(g_cb, 'F', nil);
    if (!bfin_pass(src, dst, R.clear_c[0], R.cc[0])) return 0;
    R.clear_c[0] = 0;   // done by the pass: still pending, it would wipe the picture when the UI opens the canvas's pass
    if (!g_bfin.off_n++) MLOG("[fxaa] BotW: FXAA off, the picture %lux%lu drawn into the canvas %lux%lu %s", (unsigned long)src.width, (unsigned long)src.height,
                              (unsigned long)dst.width, (unsigned long)dst.height, src.width == dst.width && src.height == dst.height ? "texel for texel" : "bilinear");
    return 1;
}
/* Before every draw (mfxup_draw): the first draw after the scene that writes or reads the canvas has the canvas sharpened
   first. The FXAA / upscale draw itself only notes the scene (botw_fin_upscale). */
static void botw_fin_before(__unsafe_unretained NvShader *fs) {
    if (!g_bfin.pending) return;
    if (g_bfin.frame != atomic_load(&g_cur_frame)) { g_bfin.pending = 0; g_bfin.canvas = nil; return; }   // no such draw came: nothing owed
    if (!strncmp(fs->key, BOTW_MFXUP_KEY, 65)) return;
    uint64_t h = fin_frag_tex(fs);
    int writes = R.rt.n > 0 && R.rt.color[0] && R.rt.color[0]->serial == g_bfin.canvas_ser, reads = h && h_ser(h) == g_bfin.canvas_ser;
    if (!writes && !reads) return;
    int pct = atomic_load_explicit(&g_fin_sharpen, memory_order_relaxed);
    id<MTLTexture> c = g_bfin.canvas; g_bfin.pending = 0; g_bfin.canvas = nil;
    if (!pct || !c) return;
    if (!g_bfin.scratch || g_bfin.scratch.width != c.width || g_bfin.scratch.height != c.height || g_bfin.scratch.pixelFormat != c.pixelFormat) {
        if (g_bfin.scratch) defer_obj_release(g_bfin.scratch);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.pixelFormat width:c.width height:c.height mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        g_bfin.scratch = [g_dev newTextureWithDescriptor:td]; if (g_bfin.scratch) res_add(g_bfin.scratch);
    }
    if (!g_bfin.scratch) return;
    rp_end(); blit_close(); rs_sync();
    pt_mark(g_cb, 'F', nil);
    if (!fin_pass(c, g_bfin.scratch, pct)) return;
    id<MTL4ComputeCommandEncoder> b = blit();
    enc_b_copy_texture(b, g_bfin.scratch, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(c.width, c.height, 1), c, 0, 0, MTLOriginMake(0, 0, 0));
    blit_close();
    if (!g_bfin.sharp_n++ || g_bfin.sharp_n % 3600 == 0)
        MLOG("[fxaa] BotW: sharpening %d%% on the canvas %lux%lu before the %s (%llu frames)", pct, (unsigned long)c.width, (unsigned long)c.height,
             writes ? "first UI draw" : "copy to the screen", (unsigned long long)g_bfin.sharp_n);
}
/* The settings panel: 1 while MetalFX replaces the FXAA / upscale draw (in the last two seconds of frames), so the FXAA
   row says it is not used. */
int dm_gfx_botw_fxaa_replaced(void) {
    uint64_t f = g_bfin.replaced_frame;
    return f && atomic_load(&g_cur_frame) - f < 120;
}
