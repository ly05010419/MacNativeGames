// An attachment-less draw of four full-screen triangles whose fragment shader stores into the layer its primitive ID names (Fire Emblem Fortune's Weave's deinterleaved
// ambient occlusion, b6726be4afc44691): every layer of the array image must be written, through the host's own draw path
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t image = {.magic = MAGIC('T','E','X',0), .serial = 99901};
static prog_t program = {.serial = 99902};
int main(void) { @autoreleasepool {
    mtl_init();
    image.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D_ARRAY, .format = NVN_FORMAT_RGBA8, .width = 8, .height = 8, .depth = 4, .levels = 1, .samples = 1};
    MTLTextureDescriptor *td = tex_desc(&image, 0); td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> texture = [g_dev newTextureWithDescriptor:td]; res_add(texture); mtl_tex_init(&image); serial_put(image.serial, texture); mtl_reg_image(701, &image, NULL);
    NSError *error = nil;
    // A .metallib loaded as the host loads game shaders (lib_from_file)
    static const char *src = "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct VO { float4 p [[position, invariant]]; float s [[point_size]]; };\n"   // a point size output, as the game's translated vertex shader has
        "vertex VO v(uint n [[vertex_id]]) { int k = int(n % 3); VO o; o.p = float4(float(k & 2) * 2 - 1, 1 - float((k * 2) & 2) * 2, 0, 1); o.s = 1; return o; }\n"
        "struct A { texture2d_array<float, access::write> image [[id(0)]]; };\n"
        "fragment void f(float4 p [[position]], uint pid [[primitive_id]], constant A &a [[buffer(0)]]) { a.image.write(float4(float(pid + 1) * 0.25, 0, 0, 1), uint2(p.xy), pid); }\n";
    char dir[] = "/tmp/t_image_primitive_id.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *ms = [NSString stringWithFormat:@"%s/p.metal", dir], *air = [NSString stringWithFormat:@"%s/p.air", dir], *ml = [NSString stringWithFormat:@"%s/p.metallib", dir];
    [@(src) writeToFile:ms atomically:NO encoding:NSUTF8StringEncoding error:nil];
    for (NSArray *args in @[@[@"-sdk", @"macosx", @"metal", @"-c", ms, @"-o", air], @[@"-sdk", @"macosx", @"metallib", air, @"-o", ml]]) {   // the compiler with its arguments, no shell
        NSTask *t = [NSTask new]; t.executableURL = [NSURL fileURLWithPath:@"/usr/bin/xcrun"]; t.arguments = args;
        if (![t launchAndReturnError:&error]) { fprintf(stderr, "metal compile: %s\n", error.localizedDescription.UTF8String); return 1; }
        [t waitUntilExit]; if (t.terminationStatus) { fprintf(stderr, "metal compile failed\n"); return 1; }
    }
    id<MTLLibrary> lib = lib_from_file(ml, &error);
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    NvProg *pr = [NvProg new]; pr->sh[0] = [NvShader new]; pr->sh[0]->fn = fn_spec(lib, @"v", nil, &error);   // through the Metal 4 bases, as the host loads shaders
    NvShader *fs = [NvShader new]; pr->sh[1] = fs; fs->stage = 1; fs->fn = fn_spec(lib, @"f", nil, &error);
    fs->rf.reads_prim_id = 1;   // as the reflection records it (raster.primitive_id): pso_build_ex builds the pipeline whole
    fs->rf.color_known = 1; fs->rf.color_mask = 0; fs->rf.nb = 1; fs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_IMG, .aid = 0, .reg = 0, .rw = 1, .dtt = MTLTextureType2DArray};
    fs->ae = [fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len = fs->ae.encodedLength;
    serial_put(program.serial, pr);
    state_reset(); memset(&R, 0, sizeof R); mtl_set_frame(1); mtl_sub_begin();
    S.img[1][0] = 701; S.has_vp = 1; S.vp[0][0] = 0; S.vp[0][1] = 0; S.vp[0][2] = 8; S.vp[0][3] = 8;
    struct { op_hdr rh; p_set_rt rt; op_hdr ph; p_bind_program pb; op_hdr dh; p_draw d; op_hdr end; } code = {0};
    code.rh = (op_hdr){OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt)}; code.rt.n = 0;   // no render target: an attachment-less pass at the viewport's size
    code.ph = (op_hdr){OP_BIND_PROGRAM, sizeof(op_hdr) + sizeof(p_bind_program)}; code.pb = (p_bind_program){&program, 3};
    code.dh = (op_hdr){OP_DRAW, sizeof(op_hdr) + sizeof(p_draw)}; code.d = (p_draw){NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 12};
    code.end = (op_hdr){OP_END, sizeof(op_hdr)};
    mtl_exec((const uint8_t *)&code);
    rp_end();
    id<MTLBuffer> pixels = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(pixels);
    for (unsigned layer = 0; layer < 4; layer++) enc_b_copy_texture_buffer(blit(), texture, layer, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(8, 8, 1), pixels, 256 * layer, 32, 256);
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completes");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
    for (unsigned layer = 0; layer < 4; layer++) {
        const uint8_t *p = (const uint8_t *)pixels.contents + 256 * layer; uint8_t want = (uint8_t)((layer + 1) * 64 - (layer == 3));
        int good = 1; for (unsigned i = 0; i < 64; i++) good &= p[4 * i] >= want - 1 && p[4 * i] <= want + 1;
        char label[96]; snprintf(label, sizeof label, "layer %u written by primitive %u", layer, layer); CHECK(good, label);
        printf("layer %u: first pixel %u,%u,%u,%u\n", layer, p[0], p[1], p[2], p[3]);
    }
    printf("IMAGE_PRIMITIVE_ID %d checks, %d failed\n", checks, failed); return failed != 0;
} }
