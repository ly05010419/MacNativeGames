// Feedback snapshots and their per-draw argument substitution include separate texture slots and read-only fragment images; a snapshot is reused until the game's next barrier,
// and a draw that only samples an attachment nothing in its pass has written reads it directly. With the directory test_dmsh_fetch.py --fixture writes, a draw that samples the
// attachment at its own pixel through a projective sample reads it from the tile (fb_tile_plan): a snapshot while its pipeline builds, then none after a barrier, the pass stays
// open, and the GPU reads what an earlier draw of the pass wrote; not with a viewport smaller than the target. The same for a sample at the draw's pixel position times a uniform
// 1 / the target's size, set by an inline update (Splatoon Raiders' ink stamps), on an RG8 attachment sampled through (red, green, zero, one): the tile read fills the missing
// channels as a sample does.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static id<MTLTexture> target_format(tex_t *t, uint64_t serial, int format, int levels) {
    *t = (tex_t){.magic=MAGIC('T','E','X',0), .serial=serial};
    t->d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=format,
        .width=4, .height=4, .depth=1, .levels=levels, .samples=1, .flags=format == NVN_FORMAT_R32UI ? NVN_TEXTURE_FLAGS_IMAGE : 0};
    MTLTextureDescriptor *d = tex_desc(t, 0); d.storageMode = MTLStorageModeShared;
    if (format == NVN_FORMAT_R32UI) d.usage |= MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:d];
    CHECK(texture != nil, "render target created");
    res_add(texture); mtl_tex_init(t); serial_put(serial, texture);
    return texture;
}
static id<MTLTexture> target(tex_t *t, uint64_t serial) {
    return target_format(t, serial, NVN_FORMAT_RGBA8, 1);
}
static void clear(MTLClearColor color) {
    R.clear_c[0] = 1; R.cc[0] = color;
    CHECK(rp_begin() != nil, "production render pass opens");
}
static void sample(NvShader *fs, id<MTLRenderPipelineState> pipeline) {
    clear(MTLClearColorMake(0, 0, 0, 1));
    enc_r_pso(g_enc, pipeline);
    enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1});
    CHECK(encode_args((id<MTLCommandEncoder>)g_enc, fs, 1, 0), "production fragment arguments encoded");
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    rp_end();
}
static void readback(id<MTLTexture> t, id<MTLBuffer> b, unsigned offset) {
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), b, offset, 256, 1024);
}
static void pixels(id<MTLBuffer> b, unsigned offset, const uint8_t want[4], const char *label) {
    int valid = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
        valid &= !memcmp((const uint8_t *)b.contents + offset + y * 256 + x * 4, want, 4);
    CHECK(valid, label);
}
static void slot_case(NvProg *pr, id<MTLRenderPipelineState> pipeline, int slot, int index) {
    int before = failed, tid = 600 + index;
    NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = slot;
    int image = fs->rf.b[0].kind == NVN_RB_IMG;
    tex_t source, output;
    id<MTLTexture> original = target_format(&source, 99700 + index * 2, image ? NVN_FORMAT_R32UI : NVN_FORMAT_RGBA8, 1);
    id<MTLTexture> destination = target(&output, 99701 + index * 2);
    mtl_reg_tex(tid, &source, NULL);
    id<MTLBuffer> result = [g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source;
    if (image) S.img[1][slot] = tid; else S.tex[1][slot] = tid;
    clear(MTLClearColorMake(image ? 17 : 1, 0, 0, 1));
    if (image) {
        fs->rf.b[0].rw = 1; fb_prepare(pr);
        CHECK(!g_fb_n && g_enc, "writes stay on the original attachment");
        fs->rf.b[0].rw = 0; fs->rf.b[0].atom = 1; fb_prepare(pr);
        CHECK(!g_fb_n && g_enc, "atomics stay on the original attachment");
        fs->rf.b[0].atom = 0;
    }
    fb_prepare(pr);
    CHECK(!g_enc, "snapshot closes the preceding render pass");
    CHECK(g_fb_n == 1, "feedback attachment receives a snapshot");
    CHECK(g_fb_n == 1 && g_fb_src[0] == original && g_fb_use[0] != original,
          "snapshot substitutes a distinct texture for the registered source");
    rp_end();
    // Overwrite the attachment after the snapshot, then render the sampled value
    // into a distinct target. This makes a missing snapshot fail deterministically
    // even on GPUs that happen to tolerate sampling an attached render target.
    clear(MTLClearColorMake(image ? 29 : 0, 1, 0, 1)); rp_end();
    R.rt.color[0] = &output;
    sample(fs, pipeline);
    uint64_t bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
    CHECK(g_fb_n == 1 && bound == g_fb_rid[0] && bound != original.gpuResourceID._impl,
          "production argument buffer contains the snapshot resource ID");
    readback(destination, result, 0);
    if (image) {
        // A different slot can write the same resource: a snapshot already available for a reader must never redirect a writer.
        fs->rf.b[0].rw = 1; args_dirty_all();
        sample(fs, pipeline);
        bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
        CHECK(bound == original.gpuResourceID._impl, "a writable image bypasses an available snapshot");
        fs->rf.b[0].rw = 0; args_dirty_all();
    }
    // With a different attachment the following draw restores the original binding.
    fb_prepare(pr);
    CHECK(g_fb_n == 0, "following draw needs no snapshot");
    sample(fs, pipeline);
    bound = *(uint64_t *)((uint8_t *)fs->ab_buf.contents + fs->ab_off);
    CHECK(bound == original.gpuResourceID._impl, "following draw restores the original resource ID");
    readback(destination, result, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    pixels(result, 0, (const uint8_t[]){255, 0, 0, 255}, "first draw reads the red pre-overwrite snapshot");
    pixels(result, 1024, (const uint8_t[]){0, 255, 0, 255}, "following draw reads the green original attachment");
    printf("feedback slot %d: %s\n", slot, before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_tex_release(&output);
}
// A registered image view names its own level. A shader that needs a different pixel class keeps its normal image binding.
static void image_view_case(NvProg *pr) {
    const int tid = 661;
    tex_t source;
    id<MTLTexture> original = target_format(&source, 99796, NVN_FORMAT_R32UI, 2);
    view_t level1 = {.set_levels=1, .base_level=1, .nlevels=1};
    mtl_reg_image(tid, &source, &level1);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    nvn_rbind *b = &pr->sh[1]->rf.b[0];
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.img[1][b->reg] = tid;
    clear(MTLClearColorMake(11, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a level-1 image does not snapshot a level-0 attachment");
    rp_end();
    R.rt.has_cview[0] = 1; R.rt.cview[0] = level1;
    clear(MTLClearColorMake(17, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_src[0].parentTexture == original &&
          g_fb_src[0].parentRelativeLevel == 1 && g_fb_use[0].width == 2,
          "a level-1 image snapshots its level-1 attachment");
    rp_end();
    b->dpf = NVN_DPF_SINT;
    clear(MTLClearColorMake(29, 0, 0, 1));
    os_unfair_lock_lock(&g_rmu);
    int real; uint64_t rid; id<MTLTexture> signed_view = img_binding(tid, b, &rid, &real);
    os_unfair_lock_unlock(&g_rmu);
    CHECK(real && signed_view.pixelFormat == MTLPixelFormatR32Sint, "the image binding can reinterpret R32Uint as SINT");
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a SINT-class read of the R32Uint attachment keeps its reinterpreted image binding");
    rp_end(); b->dpf = NVN_DPF_UINT;
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "image view submission completed without error");
    CHECK(serial_get(source.serial) == original, "the serial table still holds the image target");   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns original (serial_put's CFBridgingRetain) until mtl_tex_release below, not a leak
    mtl_tex_release(&source);
    CHECK(serial_get(source.serial) == nil, "releasing the image target removes its serial entry");
}
// Mip-chain passes: a draw rendering one level of a texture whose view it samples reads a view without the attached level. Downsampling (the level below) keeps the view of the
// levels below; a sampler whose LOD clamp pins one level (fb_pinned_level) reads that level alone, also above the attachment's (an upsample: level n from n + 1) and while
// rendering the view's base level, which the levels below could not reach (the view clamped the read to n - 1, or none was made)
static void mip_chain_case(NvProg *pr) {
    const int tid = 671, sid = 672; NvShader *fs = pr->sh[1]; nvn_rbind keep[2] = {fs->rf.b[0], fs->rf.b[1]}; int keep_nb = fs->rf.nb;
    fs->rf.nb = 2; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D}; fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_SAMPT, .reg=0, .aid=1};
    tex_t chain; id<MTLTexture> original = target_format(&chain, 99800, NVN_FORMAT_RGBA8, 3);   // 4x4, 2x2, 1x1
    mtl_reg_tex(tid, &chain, NULL);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &chain; R.rt.has_cview[0] = 1; S.tex[1][0] = tid | (uint64_t)sid << 20;
    struct { int level, lod_min, lod_max, minf, want_n, want_level, want_levels; const char *label; } c[] = {
        {1, 2, 2, 5, 1, 2, 1, "an upsample's sampler pins the level above: that level alone"},
        {1, 0, 0, 5, 1, 0, 1, "a sampler pinning the level below: that level alone"},
        {1, -1000, 1000, 5, 1, 0, 1, "a sampler that pins no level: the levels below"},
        {1, 2, 2, 1, 1, 0, 1, "no mip filter: the levels below"},
        {0, 1, 1, 5, 1, 1, 1, "rendering the base level, the sampler pins level 1: that level alone"},
        {0, -1000, 1000, 5, 0, 0, 0, "rendering the base level, no level pinned: left as is"},
    };
    for (unsigned i = 0; i < sizeof c / sizeof *c; i++) {
        samp_par sp = {.minf = (uint8_t)c[i].minf, .magf = 1, .ws = 2, .wt = 2, .wr = 2, .lod_min = c[i].lod_min, .lod_max = c[i].lod_max, .aniso = 1}; mtl_reg_samp(sid, &sp);
        R.rt.cview[0] = (view_t){.set_levels=1, .base_level=c[i].level, .nlevels=1};
        clear(MTLClearColorMake(0, 0, 0, 1)); fb_prepare(pr);
        id<MTLTexture> use = g_fb_n ? g_fb_use[0] : nil;
        printf("%s: %d views, level %lu+%lu\n", c[i].label, g_fb_n, (unsigned long)use.parentRelativeLevel, (unsigned long)use.mipmapLevelCount);
        CHECK(g_fb_n == c[i].want_n && (!c[i].want_n || (use.parentTexture == original && (int)use.parentRelativeLevel == c[i].want_level && (int)use.mipmapLevelCount == c[i].want_levels)), c[i].label);
        rp_end();
    }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "mip-chain submission completed without error");
    fs->rf.nb = keep_nb; fs->rf.b[0] = keep[0]; fs->rf.b[1] = keep[1];
    mtl_tex_release(&chain);
}
// The rule without a listed shader: copy once, reuse until a barrier, end the pass when an earlier draw of it read the attachment directly.
static void reuse_case(NvProg *pr) {
    int before = failed, tid = 650; NvShader *fs = pr->sh[1]; fs->rf.b[0].reg = 0;
    tex_t source; target(&source, 99790);
    mtl_reg_tex(tid, &source, NULL);   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the texture (serial_put's CFBridgingRetain) until mtl_tex_release below, not a leak
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid;
    S.chmask.mask = 0; clear(MTLClearColorMake(1, 0, 0, 1));
    uint64_t copies = g_fb_count;
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a draw that writes nothing reads an unwritten attachment directly");
    S.chmask.mask = 0xffffffffu;
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1, "a writing draw takes a snapshot");
    id<MTLTexture> first = g_fb_use[0];
    clear(MTLClearColorMake(0, 1, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && g_enc && g_fb_count == copies + 1 && g_fb_use[0] == first, "the next writing draw reuses the snapshot without ending the pass");
    S.chmask.mask = 0; rp_end(); clear(MTLClearColorMake(0, 1, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 0 && g_enc, "a reading draw first in a pass reads directly");
    S.chmask.mask = 0xffffffffu;
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1, "a writing draw after a direct read ends the pass and reuses the snapshot");
    g_fb_bar++;
    clear(MTLClearColorMake(0, 0, 1, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 2, "a game barrier makes the next writing draw take a fresh snapshot");
    rp_end();
    // The depth attachment read, then written in the same pass (only serials matter to the rule: the attachment is named after the pass opens)
    { tex_t output, depth; target(&output, 99791); target(&depth, 99792); mtl_reg_tex(651, &depth, NULL);   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns both textures (serial_put's CFBridgingRetain) until mtl_tex_release below, not a leak
      R.rt.color[0] = &output; R.rt.depth = NULL; S.tex[1][0] = 651; S.chmask.mask = 0xffffffffu; S.ds.dt = 1; S.ds.dw = 0;
      clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth;
      fb_prepare(pr);
      CHECK(g_enc != nil, "a draw that reads the depth attachment stays in its pass");
      fb_prepare(pr);
      CHECK(g_enc != nil, "so does the next one, which writes no depth");
      S.ds.dw = 1; fb_prepare(pr);
      CHECK(!g_enc, "a draw writing depth after a read of it ends the pass");
      R.rt.depth = NULL; clear(MTLClearColorMake(0, 0, 0, 1)); rp_end();   // that draw read it too, and began the pass it would open: the next pass is a clean one
      clear(MTLClearColorMake(0, 0, 0, 1)); R.rt.depth = &depth; S.tex[1][0] = 0;
      fb_prepare(pr);
      CHECK(g_enc != nil, "a draw writing depth with no read of it in its pass stays");
      R.rt.depth = NULL; S.ds.dt = S.ds.dw = 0; rp_end(); mtl_tex_release(&output); mtl_tex_release(&depth); }
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    printf("feedback reuse: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
}
static id<MTLLibrary> lib_file(NSString *path) {
    NSError *e = nil; NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&e];
    id<MTLLibrary> l = src ? [g_dev newLibraryWithSource:src options:nil error:&e] : nil;
    if (!l) fprintf(stderr, "%s: %s\n", path.UTF8String, e.localizedDescription.UTF8String);
    return l;
}
// Breath of the Wild's decals: the G-buffer target cleared red, an earlier draw of the pass writes green into its left half, then a draw that samples the target at its screen varying
// writes the sample's channels rotated (green from blue, blue from red, red from green): red where it read green, blue where it read red
static void tile_case(const char *dir) {
    int before = failed, tid = 660; NSString *d = @(dir);
    g_msl_dir = d;   // bf_fn loads <key>_bf.metallib from here
    NSData *fj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"facts.json"]];
    NSDictionary *facts = fj ? [NSJSONSerialization JSONObjectWithData:fj options:0 error:nil] : nil;
    id<MTLLibrary> vl = lib_file([d stringByAppendingPathComponent:@"fb_vs.metal"]), fl = lib_file([d stringByAppendingPathComponent:@"fb_fs.metal"]);
    CHECK(facts && vl && fl, "the fixture's shaders compile");
    if (!facts || !vl || !fl) return;
    NvProg *pr = [NvProg new]; NvShader *vs = [NvShader new], *fs = [NvShader new]; pr->sh[0] = vs; pr->sh[1] = fs;
    vs->stage = 0; strcpy(vs->key, "0123456789abcdef_b"); vs->fn = [vl newFunctionWithName:@"main0"]; vs->rf.pos_attr = -1;
    for (NSNumber *k in facts[@"screen"]) vs->rf.scr_locn |= 1u << k.intValue;
    fs->stage = 1; strcpy(fs->key, "fedcba9876543210_b"); fs->fn = [fl newFunctionWithName:@"main0"];
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D};
    for (NSDictionary *c in facts[@"calls"]) { int i = fs->rf.nfc++; fs->rf.fc_reg[i] = (uint8_t)[c[@"tex_reg"] intValue]; fs->rf.fc_locn[i] = (uint8_t)[c[@"varying_locn"] intValue]; if ([c[@"proj"] intValue]) fs->rf.fc_proj |= 1u << i; }
    for (NSDictionary *o in facts[@"outputs"]) fs->rf.fo_color[fs->rf.nfo++] = (uint8_t)[o[@"color"] intValue];
    fs->rf.color_known = 1; fs->rf.color_mask = 1;
    CHECK((vs->rf.scr_locn >> 4 & 1) && fs->rf.nfc == 1 && fs->rf.fc_proj == 1 && fs->rf.fc_locn[0] == 4, "the fixture's facts: screen varying 4 (and 5), one projective sample of 4");
    NSError *e = nil;
    id<MTLLibrary> gl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(0, 1, 0, 1); }\n" options:nil error:&e];
    MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new];
    gd.vertexFunction = [gl newFunctionWithName:@"v"]; gd.fragmentFunction = [gl newFunctionWithName:@"f"]; gd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> green = gl ? [g_dev newRenderPipelineStateWithDescriptor:gd error:&e] : nil;
    CHECK(green != nil, "the green pipeline compiles");
    tex_t source; id<MTLTexture> attachment = target(&source, 99795);
    mtl_reg_tex(tid, &source, NULL);
    id<MTLBuffer> zero = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(zero);   // the variant's argument buffer: the tile read leaves its texture and sampler unread
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid; S.chmask.mask = 0xffffffffu;
    S.has_vp = 1; S.vp[0][0] = 0; S.vp[0][1] = 0; S.vp[0][2] = 4; S.vp[0][3] = 4;
    uint64_t copies = g_fb_count, waits = g_fb_tile_wait, tiles = g_fb_tile_draws;
    clear(MTLClearColorMake(1, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1 && g_fb_tile_wait == waits + 1 && g_fb_tseq != g_draw_seq, "while its pipeline builds, the draw takes a snapshot");
    for (int i = 0; i < 1000; i++) { os_unfair_lock_lock(&g_fb_tp_lock); int done = g_fb_tp_n && g_fb_tp[0].done; os_unfair_lock_unlock(&g_fb_tp_lock); if (done) break; usleep(10000); }
    CHECK(g_fb_tp_n == 1 && g_fb_tp[0].done && g_fb_tp[0].ps, "the tile-reading pipeline is built in the background");
    g_fb_bar++;   // a game barrier: a snapshot would have to be taken again
    clear(MTLClearColorMake(1, 0, 0, 1));
    id first = g_enc;
    enc_r_pso(g_enc, green); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 2, 4});
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    fb_prepare(pr);
    CHECK(g_enc == first && g_fb_n == 0 && g_fb_count == copies + 1 && g_fb_tseq == g_draw_seq && g_fb_tfn && g_fb_tile_draws == tiles + 1 && g_fb_tdesc.fbmask == 1 && g_fb_tdesc.fx[0] == 0,
          "after a barrier the draw reads the tile: no snapshot, the pass stays open");
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> ps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(ps == g_fb_tp[0].ps, "the draw's pipeline is the one built in the background");
    if (ps) { enc_r_pso(g_enc, ps); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, zero.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024);
    // An image and a sampled texture can share a slot number. Only the sampled binding reads the tile.
    fs->rf.b[1] = (nvn_rbind){.kind=NVN_RB_IMG, .reg=0, .aid=1, .dtt=MTLTextureType2D};
    fs->rf.nb = 2; S.img[1][0] = tid; g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_tseq == g_draw_seq && g_fb_n == 1 && g_fb_src[0] == attachment && g_fb_count == copies + 2,
          "sampled slot 0 reads the tile while image slot 0 snapshots the same attachment");
    rp_end(); fs->rf.nb = 1; S.img[1][0] = 0;
    S.vp[0][2] = 2; g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1));
    fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_tseq != g_draw_seq, "with a viewport smaller than the target the draw takes a snapshot");
    rp_end();
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    int left = 1, right = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = (const uint8_t *)result.contents + y * 256 + x * 4;
        if (x < 2) left &= p[0] == 255 && p[1] == 0 && p[2] == 0; else right &= p[0] == 0 && p[1] == 0 && p[2] == 255; }
    CHECK(left, "where the earlier draw of the pass wrote green, the tile read gives green");
    CHECK(right, "elsewhere it gives the cleared red");
    printf("feedback tile read: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source);
}
// The channel mappings a tile read can stand for: the format's channels in place, the missing ones as they are or as the 0, 0, 1 a sample fills in
static void swizzle_case(void) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm width:4 height:4 mipmapped:NO]; d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> rg = [g_dev newTextureWithDescriptor:d]; d.pixelFormat = MTLPixelFormatRGBA8Unorm; id<MTLTexture> rgba = [g_dev newTextureWithDescriptor:d];
    #define SW(t, r, g, b, a) [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a)]
    CHECK(sw_tile(rg) && sw_tile(SW(rg, Red, Green, Zero, One)) && sw_tile(SW(rg, Red, Green, Blue, One)), "an RG8 view through (red, green, zero, one) reads like the tile");
    CHECK(!sw_tile(SW(rg, Red, Green, One, One)) && !sw_tile(SW(rg, Green, Red, Zero, One)) && !sw_tile(SW(rgba, Red, Green, Blue, One)), "not with other channels, swapped ones or alpha 1 of RGBA8");
    #undef SW
}
// Splatoon Raiders' ink stamps (GitHub #1063): the RG8 map cleared red, an earlier draw of the pass writes green into its left half, then a draw that samples the map at gl_FragCoord
// times c[6].xy writes red where it read green, black where it read red, and green where a missing channel read other than 0, 0, 1. Memory holds a scale of 1/8; the inline update
// of this submission sets 1/4, the map's size: the draw is proven only once that update is queued
static void fpos_case(const char *dir) {
    int before = failed, tid = 661; NSString *d = @(dir);
    g_msl_dir = d;
    NSData *fj = [NSData dataWithContentsOfFile:[d stringByAppendingPathComponent:@"pos.json"]];
    NSDictionary *facts = fj ? [NSJSONSerialization JSONObjectWithData:fj options:0 error:nil] : nil;
    id<MTLLibrary> vl = lib_file([d stringByAppendingPathComponent:@"fb_vs.metal"]), fl = lib_file([d stringByAppendingPathComponent:@"fb_pos.metal"]);
    CHECK(facts && vl && fl, "the pixel-position fixture compiles");
    if (!facts || !vl || !fl) return;
    NvProg *pr = [NvProg new]; NvShader *vs = [NvShader new], *fs = [NvShader new]; pr->sh[0] = vs; pr->sh[1] = fs;
    vs->stage = 0; strcpy(vs->key, "0123456789abcdee_b"); vs->fn = [vl newFunctionWithName:@"main0"]; vs->rf.pos_attr = -1;
    fs->stage = 1; strcpy(fs->key, "fedcba9876543211_b"); fs->fn = [fl newFunctionWithName:@"main0"];
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .reg=0, .aid=0, .dtt=MTLTextureType2D};
    for (NSDictionary *c in facts[@"calls"]) { int i = fs->rf.nfc++; NSArray *f = c[@"frag_pos"]; fs->rf.fc_reg[i] = (uint8_t)[c[@"tex_reg"] intValue]; fs->rf.fc_locn[i] = 0xff;
        if (f.count == 2) { fs->rf.fc_fpos |= 1u << i; for (int q = 0; q < 2; q++) fs->rf.fc_scale[i][q] = (uint32_t)([f[q][0] intValue] << 16 | [f[q][1] intValue] << 2 | [f[q][2] intValue]); } }
    for (NSDictionary *o in facts[@"outputs"]) fs->rf.fo_color[fs->rf.nfo++] = (uint8_t)[o[@"color"] intValue];
    fs->rf.color_known = 1; fs->rf.color_mask = 1;
    CHECK(fs->rf.nfc == 1 && fs->rf.fc_fpos == 1 && fs->rf.fc_scale[0][0] == (6 << 2) && fs->rf.fc_scale[0][1] == (6 << 2 | 1), "the fixture's facts: one sample at the pixel position times c[6].xy");
    NSError *e = nil;
    id<MTLLibrary> gl = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "fragment float4 f() { return float4(0, 1, 0, 1); }\n" options:nil error:&e];
    MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new];
    gd.vertexFunction = [gl newFunctionWithName:@"v"]; gd.fragmentFunction = [gl newFunctionWithName:@"f"]; gd.colorAttachments[0].pixelFormat = MTLPixelFormatRG8Unorm;
    id<MTLRenderPipelineState> green = gl ? [g_dev newRenderPipelineStateWithDescriptor:gd error:&e] : nil;
    CHECK(green != nil, "the green pipeline compiles");
    tex_t source; id<MTLTexture> attachment = target_format(&source, 99796, NVN_FORMAT_RG8, 1);
    mtl_reg_tex(tid, &source, NULL);
    { void *tv = NULL; tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; MTLTextureSwizzleChannels w = t.swizzle;
      printf("feedback pixel position: the RG8 map is sampled through channel mapping %d %d %d %d\n", (int)w.red, (int)w.green, (int)w.blue, (int)w.alpha); }
    float *mem = NULL; if (posix_memalign((void **)&mem, (size_t)getpagesize(), (size_t)getpagesize())) abort(); memset(mem, 0, (size_t)getpagesize());
    mem[24] = mem[25] = 0.125f; mtl_pool_add(mem, (uint64_t)getpagesize());   // c[6].xy in memory: 1/8
    id<MTLBuffer> ub = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(ub); ((float *)ub.contents)[20] = 1;   // the support buffer's render scale (_m5[0].x), and c[6] read by the GPU
    ((float *)ub.contents)[24] = ((float *)ub.contents)[25] = 0.25f;
    id<MTLArgumentEncoder> ae = [fs->fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [g_dev newBufferWithLength:MAX(ae.encodedLength, 256ul) options:MTLResourceStorageModeShared]; res_add(args);
    [ae setArgumentBuffer:args offset:0]; [ae setBuffer:ub offset:0 atIndex:2]; [ae setBuffer:ub offset:0 atIndex:3]; [ae setBuffer:ub offset:0 atIndex:4];
    id<MTLBuffer> result = [g_dev newBufferWithLength:1024 options:MTLResourceStorageModeShared]; res_add(result);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &source; S.tex[1][0] = tid; S.chmask.mask = 0xffffffffu;
    S.ubo[1][0] = (p_bind_buf){.stage=1, .index=0, .addr=(uint64_t)mem, .size=256};
    S.has_vp = 1; S.vp[0][0] = 0; S.vp[0][1] = 0; S.vp[0][2] = 2; S.vp[0][3] = 2;   // a viewport smaller than the target does not matter to gl_FragCoord
    uint64_t copies = g_fb_count, waits = g_fb_tile_wait, tiles = g_fb_tile_draws;
    clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 1 && g_fb_tile_wait == waits, "with a scale of 1/8 the draw is not its pixel's: a snapshot, no tile-reading pipeline");
    float quarter[2] = {0.25f, 0.25f}; mtl_sub_update(quarter, (uint64_t)(mem + 24), 8);
    g_fb_bar++; clear(MTLClearColorMake(1, 0, 0, 1)); fb_prepare(pr);
    CHECK(g_fb_n == 1 && !g_enc && g_fb_count == copies + 2 && g_fb_tile_wait == waits + 1, "the inline update's 1/4 proves it; while the pipeline builds, a snapshot");
    for (int i = 0; i < 1000; i++) { os_unfair_lock_lock(&g_fb_tp_lock); int done = 1; for (int k = 0; k < g_fb_tp_n; k++) done &= g_fb_tp[k].done; os_unfair_lock_unlock(&g_fb_tp_lock); if (done) break; usleep(10000); }
    g_fb_bar++;
    clear(MTLClearColorMake(1, 0, 0, 1));
    id first = g_enc;
    enc_r_pso(g_enc, green); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 2, 4});
    enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3);
    fb_prepare(pr);
    CHECK(g_enc == first && g_fb_n == 0 && g_fb_tseq == g_draw_seq && g_fb_tfn && g_fb_tile_draws == tiles + 1 && g_fb_tdesc.fbmask == 1 && g_fb_tdesc.fx[0] == 0,
          "after a barrier the draw reads the tile: no snapshot, the pass stays open");
    g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc;
    id<MTLRenderPipelineState> ps = pso_get(pr, nil);
    g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz);
    CHECK(ps != nil, "the tile-reading pipeline is there");
    if (ps) { enc_r_pso(g_enc, ps); enc_r_viewport(g_enc, (MTLViewport){0, 0, 4, 4, 0, 1}); enc_r_scissor(g_enc, (MTLScissorRect){0, 0, 4, 4}); enc_r_args(g_enc, MTLRenderStageFragment, args.gpuAddress, 0); enc_r_draw(g_enc, MTLPrimitiveTypeTriangle, 0, 3); }
    rp_end();
    enc_b_copy_texture_buffer(blit(), attachment, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), result, 0, 256, 1024);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    int left = 1, right = 1;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { const uint8_t *p = (const uint8_t *)result.contents + y * 256 + x * 2;
        if (x < 2) left &= p[0] == 255 && p[1] == 0; else right &= p[0] == 0 && p[1] == 0; }
    CHECK(left, "where the earlier draw wrote green, the tile read gives green, and blue 0 and alpha 1");
    CHECK(right, "elsewhere the cleared red, with blue 0 and alpha 1");
    printf("feedback pixel-position tile read: %s\n", before == failed ? "PASS" : "FAIL");
    mtl_tex_release(&source); mtl_pool_del(mem); mtl_drain(); free(mem);
}
// A frame that snapshots more keys than the cache has entries, in several sizes (Tears of the Kingdom: 4-5 a frame), makes no texture once each size has one: a new key takes an entry
// whose texture has its format and size. Taking the entries in turn made one for nearly every snapshot (GitHub #931). Entries a later draw can still reuse are taken last
static void shape_case(void) {
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
    static const NSUInteger size[5][2] = {{64, 32}, {32, 16}, {16, 8}, {64, 32}, {8, 4}};
    NSMutableSet *made = [NSMutableSet set]; int fresh, bad = 0;
    for (int frame = 0; frame < 6; frame++) {
        g_fb_bar++;   // the game's barrier before each snapshot draw, as in Tears of the Kingdom's shrines
        for (int k = 0; k < 5; k++, g_fb_bar++) {
            fb_cache_t *e = fb_snap(1000 + frame * 5 + k, 0, 0, MTLPixelFormatRGBA8Unorm, size[k][0], size[k][1], 0, &fresh);
            bad += !e || !fresh || e->snap.width != size[k][0] || e->snap.height != size[k][1];
            if (e) [made addObject:[NSValue valueWithPointer:(__bridge void *)e->snap]];
        }
    }
    CHECK(!bad, "every new key gets a texture of its size");
    CHECK(made.count == 4, "30 snapshots of 4 sizes over 6 frames make one texture per size, not one each");
    fb_cache_t *a = fb_snap(1, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh), *b = fb_snap(2, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh);
    CHECK(a != b, "a live snapshot is not taken for another key while a stale entry is free");
    fb_cache_t *a2 = fb_snap(1, 0, 0, MTLPixelFormatRGBA8Unorm, 64, 32, 0, &fresh);
    CHECK(a2 == a && !fresh, "the live snapshot is reused until the next barrier");
    for (int i = 0; i < 8; i++) { g_fbc[i] = (fb_cache_t){0}; } g_fbc_use = 0;
}
int main(int argc, char **argv) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init(); NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i[[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
        "struct A {texture2d<float, access::read> source[[id(0)]];};\n"
        "fragment float4 f(float4 p[[position]], constant A&a[[buffer(0)]]) {return a.source.read(uint2(p.xy));}\n"
        "struct I {texture2d<uint, access::read> source[[id(0)]];};\n"
        "fragment float4 mask(float4 p[[position]], constant I&a[[buffer(0)]]) {uint v = a.source.read(uint2(p.xy)).x; return float4(v==17u, v==29u, 0, 1);}\n"
        options:nil error:&error];
    CHECK(lib != nil, "synthetic shader compiles");
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *pr = [NvProg new]; NvShader *fs = [NvShader new]; pr->sh[1] = fs;
    fs->stage = 1; strcpy(fs->key, "0123456789abcdef"); fs->fn = [lib newFunctionWithName:@"f"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_TEX, .aid=0, .dtt=MTLTextureType2D};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"v"]; d.fragmentFunction = fs->fn;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pipeline != nil, "synthetic render pipeline compiles");
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    const int slots[] = {0, 128, 255};
    for (int i = 0; i < 3; i++) slot_case(pr, pipeline, slots[i], i);
    reuse_case(pr);
    shape_case();
    mip_chain_case(pr);
    fs->fn = [lib newFunctionWithName:@"mask"];
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    fs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .aid=0, .dtt=MTLTextureType2D, .dpf=NVN_DPF_UINT};
    d.fragmentFunction = fs->fn;
    pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pipeline != nil, "integer image pipeline compiles");
    if (pipeline) slot_case(pr, pipeline, 3, 3);
    image_view_case(pr);
    swizzle_case();
    if (argc == 2) { tile_case(argv[1]); fpos_case(argv[1]); }
    printf("feedback snapshot: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
