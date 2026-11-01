// FXAA off and sharpening (Mac, TOTK 1.4.3; user 2026-10-30, after UltraCam's "Graphics, FXAA" and a fork's sharpening).
// Game-specific and never needed to run the game; included by nvn_mtl.m just before mfxup_draw, which calls fin_draw first.
// The game's last 3D pass is FXAA 22a61cb1 (FXAA 3.11 console: the centre and four corner texels, an early return of the
// centre where the contrast is low): it reads the tone-mapped picture and writes an image of the same size that the next
// pass (the game's EASU-style upscale ac58fc18 / a3a570eb, or MetalFX in its place) reads into the canvas before the UI.
// - FXAA off: that draw is not encoded; before the reader, our full-screen pass writes the tone-mapped picture into FXAA's
//   image texel for texel (or sharpened, below), as FXAA's own early return does for every pixel.
// - Sharpening: FSR 1's RCAS (the same pass as totk_taa.m's up_copy_ps; MIT, games/totk/third_party/licenses/AMD-FidelityFX-
//   MIT.txt) at 25 to 100 % of its strength, on FXAA's image before the reader (through a scratch image of its size), so
//   the 3D picture is sharpened and the UI drawn later is not.
// Both act at once (the settings panel's effects page, graphics-fxaa.txt / graphics-sharpen.txt; "flag fxaa_off <0|1>",
// "flag sharpen_pct <0..100>"). Only a plain full-screen FXAA draw is taken (one colour target, no blending, the whole
// viewport, same size in and out); anything else keeps the game's own.
static _Atomic int g_fin_fxaa_off, g_fin_sharpen;   // FXAA off (0/1), sharpening percent (0 = none)
static struct {
    int pending, skipped; uint64_t frame, dst_ser;
    id<MTLTexture> src, dst, scratch;
    id<MTLRenderPipelineState> ps[8]; MTLPixelFormat ps_fmt[8]; int ps_pct[8];
    id<MTL4ArgumentTable> args[3]; int slot;
    uint64_t done, declined;
} g_fin;

/* RCAS at pct % (0: texel for texel) into format f; kept for the run (a handful of formats x levels). */
static id<MTLRenderPipelineState> fin_ps(MTLPixelFormat f, int pct) {
    for (int i = 0; i < 8; i++) if (g_fin.ps[i] && g_fin.ps_fmt[i] == f && g_fin.ps_pct[i] == pct) return g_fin.ps[i];
    int slot = -1; for (int i = 0; i < 8 && slot < 0; i++) if (!g_fin.ps[i]) slot = i;
    if (slot < 0) return nil;
    char src[2400];
    snprintf(src, sizeof src,
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "vertex V fin_vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
        "constant float SHARP = %.4f;\n"
        "fragment float4 fin_fs(V in [[stage_in]], texture2d<float, access::read> t [[texture(0)]]) {\n"
        "  uint2 p = uint2(in.p.xy); float4 c = t.read(p);\n"
        "  if (SHARP <= 0.0) return c;\n"
        "  uint2 hi = uint2(t.get_width() - 1, t.get_height() - 1);\n"
        "  float3 b = t.read(uint2(p.x, p.y > 0 ? p.y - 1 : 0)).rgb, d = t.read(uint2(p.x > 0 ? p.x - 1 : 0, p.y)).rgb;\n"
        "  float3 f = t.read(uint2(min(p.x + 1, hi.x), p.y)).rgb, h = t.read(uint2(p.x, min(p.y + 1, hi.y))).rgb, e = c.rgb;\n"
        "  if (!all(isfinite(b + d + f + h))) return c;\n"
        "  float3 mn = saturate(min(min(b, d), min(f, h))), mx = saturate(max(max(b, d), max(f, h))), ec = saturate(e);\n"
        "  float3 lo = max(-(min(mn, ec) / max(4.0 * mx, 1e-4)), (1.0 - max(mx, ec)) / min(4.0 * mn - 4.0, -1e-4));\n"
        "  float lobe = max(-0.1875, min(max(lo.r, max(lo.g, lo.b)), 0.0)) * SHARP;\n"
        "  return float4((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), c.a);\n"
        "}\n", pct / 100.0);
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;   // isfinite must not be folded away
    NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:o error:&e];
    if (!lib) { RED_ONCE(123, "FXAA / 锐化通道编译失败：%s", e.localizedDescription.UTF8String); return nil; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"fin_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fin_fs"];
    pd.colorAttachments[0].pixelFormat = f;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!ps) { RED_ONCE(124, "FXAA / 锐化管线建不出（格式 %lu）：%s", (unsigned long)f, e.localizedDescription.UTF8String); return nil; }
    g_fin.ps[slot] = ps; g_fin.ps_fmt[slot] = f; g_fin.ps_pct[slot] = pct;
    return ps;
}
/* One full-screen pass: in -> out (out's whole area). */
static int fin_pass(id<MTLTexture> in, id<MTLTexture> out, int pct) {
    id<MTLRenderPipelineState> ps = fin_ps(out.pixelFormat, pct); if (!ps) return 0;
    int s = g_fin.slot = (g_fin.slot + 1) % 3;
    if (!g_fin.args[s]) { MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; NSError *e = nil;
                          if (!(g_fin.args[s] = [g_dev newArgumentTableWithDescriptor:ad error:&e])) return 0; }
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> re = [g_cb renderCommandEncoderWithDescriptor:rp];
    [re barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:MTL4VisibilityOptionDevice];
    [g_fin.args[s] setTexture:in.gpuResourceID atIndex:0];
    [re setArgumentTable:g_fin.args[s] atStages:MTLRenderStageFragment];
    [re setViewport:(MTLViewport){0, 0, (double)out.width, (double)out.height, 0, 1}];
    [re setRenderPipelineState:ps];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re endEncoding];
    return 1;
}
/* The first texture this draw's fragment shader reads (FXAA reads one). */
static uint64_t fin_frag_tex(__unsafe_unretained NvShader *f) {
    for (int i = 0; i < f->rf.nb; i++) if (f->rf.b[i].kind == NVN_RB_TEX && f->rf.b[i].reg < 128 && S.tex[1][f->rf.b[i].reg]) return S.tex[1][f->rf.b[i].reg];
    return 0;
}
/* Returns 1 when this draw is not to be encoded (FXAA off). */
static int fin_draw(__unsafe_unretained NvShader *fs) {
    int off = atomic_load_explicit(&g_fin_fxaa_off, memory_order_relaxed), pct = atomic_load_explicit(&g_fin_sharpen, memory_order_relaxed);
    uint64_t frame = atomic_load(&g_cur_frame);
    if (g_fin.pending && g_fin.frame != frame) { g_fin.pending = 0; g_fin.src = nil; g_fin.dst = nil; }   // no reader came: nothing owed
    if (!off && !pct && !g_fin.pending) return 0;
#ifndef GAME_BOTW
    if (!strncmp(fs->key, "22a61cb1ec732aef", 16)) {
        g_fin.pending = 0; g_fin.src = nil; g_fin.dst = nil;
        const view_t *cv = &R.rt.cview[0]; int hv = R.rt.has_cview[0];
        uint64_t h = fin_frag_tex(fs); int tid = (int)(h & 0xfffff);
        id<MTLTexture> src = tid > 0 ? (__bridge id<MTLTexture>)tex_fast(tid) : nil;
        id<MTLTexture> dst = R.rt.n == 1 && R.rt.color[0] ? attachment_texture(R.rt.color[0], cv, hv) : nil;
        int ok = src && dst && src.textureType == MTLTextureType2D && dst.textureType == MTLTextureType2D && src.width == dst.width && src.height == dst.height &&
                 !(S.color.blend_en & 1) && !(hv && ((cv->set_levels && cv->base_level) || (cv->set_layers && cv->min_layer))) &&
                 !(S.has_vp && !(S.vp[0][0] == 0 && S.vp[0][1] == 0 && S.vp[0][2] == (float)dst.width && S.vp[0][3] == (float)dst.height)) &&
                 scissor_full(R.rt.color[0], cv, hv);
        if (!ok) { if (!g_fin.declined++) MLOG("[fxaa] FXAA 绘制不是整屏同尺寸（源 %lux%lu → 目标 %lux%lu）：保留游戏自己的", (unsigned long)src.width, (unsigned long)src.height, (unsigned long)dst.width, (unsigned long)dst.height); return 0; }
        g_fin.src = src; g_fin.dst = dst; g_fin.dst_ser = R.rt.color[0]->serial; g_fin.frame = frame; g_fin.pending = 1; g_fin.skipped = off;
        return off;
    }
#endif
    if (!g_fin.pending) return 0;
    uint64_t h = fin_frag_tex(fs); if (!h || h_ser(h) != g_fin.dst_ser) return 0;   // not the reader of FXAA's image
    g_fin.pending = 0;
    rp_end(); blit_close(); rs_sync();
    pt_mark(g_cb, 'F', nil);
    id<MTLTexture> src = g_fin.src, dst = g_fin.dst; g_fin.src = nil; g_fin.dst = nil;
    if (g_fin.skipped) fin_pass(src, dst, pct);   // FXAA off: the tone-mapped picture into FXAA's image (sharpened when asked)
    else if (pct) {   // FXAA kept: its image sharpened through a scratch image of its size
        if (!g_fin.scratch || g_fin.scratch.width != dst.width || g_fin.scratch.height != dst.height || g_fin.scratch.pixelFormat != dst.pixelFormat) {
            if (g_fin.scratch) defer_obj_release(g_fin.scratch);
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:dst.pixelFormat width:dst.width height:dst.height mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
            g_fin.scratch = [g_dev newTextureWithDescriptor:td]; if (g_fin.scratch) res_add(g_fin.scratch);
        }
        if (g_fin.scratch && fin_pass(dst, g_fin.scratch, pct)) {
            id<MTL4ComputeCommandEncoder> b = blit();
            enc_b_copy_texture(b, g_fin.scratch, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(dst.width, dst.height, 1), dst, 0, 0, MTLOriginMake(0, 0, 0));
            blit_close();
        }
    }
    if (!g_fin.done++) MLOG("[fxaa] FXAA %s，锐化 %d%%（%lux%lu）", g_fin.skipped ? "关" : "开", pct, (unsigned long)dst.width, (unsigned long)dst.height);
    return 0;
}

/* The settings panel (runtime/settings_panel.h) and the flags. */
void dm_gfx_fxaa_set(int on) { atomic_store(&g_fin_fxaa_off, !on); MLOG("[fxaa] FXAA %s", on ? "on" : "off"); }
int dm_gfx_fxaa_on(void) { return !atomic_load(&g_fin_fxaa_off); }
void dm_gfx_sharpen_set(int pct) { pct = pct < 0 ? 0 : pct > 100 ? 100 : pct; atomic_store(&g_fin_sharpen, pct); MLOG("[fxaa] sharpening %d%%", pct); }
int dm_gfx_sharpen(void) { return atomic_load(&g_fin_sharpen); }
