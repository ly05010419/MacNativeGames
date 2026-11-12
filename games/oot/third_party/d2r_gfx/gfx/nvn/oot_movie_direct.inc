/* Diagnostic only: isolate decoded movie output from NDVE reconstruction.
   Based on the matching/lifetime strategy in nxnative PR #1331, 76c20edba480db753ce7092cd2677f6c4a8daa15
   (MIT, third_party/licenses/nxnative.txt). This renderer uses an ordered compute scale instead of a render copy.
   This changes reconstruction quality, not a production correctness fix. */
static const char oot_movie_convert_key[] = "49dff010b9832251bb18e000c71a1ace470fd79dcf444a581132dd9f347becf7";
static const char oot_movie_final_key[] = "4f41ccf24f163e0dd5f7347190607a892a42575928fa6441245c43b7302d1440";
static bool oot_movie_enabled(void) {
#if !TARGET_OS_IPHONE
    static int enabled = -1;
    if (enabled < 0) { const char *e = getenv("TOTK_OOT_MOVIE_DIRECT"); enabled = e && !strcmp(e, "1"); }
    return enabled;
#else
    return 0;
#endif
}
static int oot_movie_key_is(const char *key, const char *hash) {
    return !strncmp(key, hash, 64) && (!key[64] || key[64] == '-');
}
static id<MTLTexture> oot_movie_image_handle(NvShader *cs, unsigned slot, uint64_t handle, uint64_t *serial, int write) {
    int binding = 0;
    for (int i = 0; i < cs->rf.nb; i++) {
        const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind == NVN_RB_IMG && b->reg == slot && b->rw && b->dtt == MTLTextureType2D) binding = 1;
    }
    if (!binding) return nil;
    int tid = (int)(handle & 0xfffff);
    id<MTLTexture> t = nil;
    registry_lock();
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) {
        if (write && !prepare_shader_image_locked(tid, NULL, 5, slot)) {
            registry_unlock();
            return nil;
        }
        void *raw = NULL;
        image_fast_rid(tid, &raw);
        t = (__bridge id<MTLTexture>)raw;
        if (serial) *serial = g_treg[tid].t.serial;
    }
    registry_unlock();
    if (t.textureType != MTLTextureType2D || t.sampleCount != 1 ||
        (t.pixelFormat != MTLPixelFormatRGBA8Unorm && t.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) ||
        !swizzle_eq(t.swizzle, MTLTextureSwizzleChannelsDefault)) return nil;
    return t;
}
static id<MTLTexture> oot_movie_image(NvShader *cs, unsigned slot, uint64_t *serial, int write) {
    return oot_movie_image_handle(cs, slot, S.img[5][slot], serial, write);
}
static int oot_movie_scale(id<MTLTexture> in, id<MTLTexture> out) {
    if (!in || !out || in == out ||
        !in.width || !in.height || !out.width || !out.height ||
        in.width * out.height != out.width * in.height ||
        in.textureType != MTLTextureType2D || out.textureType != MTLTextureType2D ||
        in.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        (out.pixelFormat != MTLPixelFormatRGBA8Unorm && out.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB) ||
        !(in.usage & MTLTextureUsageShaderRead) || !(out.usage & MTLTextureUsageShaderWrite)) return 0;
    static id<MTLComputePipelineState> ps;
    static id<MTL4ArgumentTable> table;
    if (!ps || !table) {
        NSError *error = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void scale_movie(texture2d<float> src [[texture(0)]], texture2d<float,access::write> dst [[texture(1)]], uint2 p [[thread_position_in_grid]]) {"
            " if(p.x>=dst.get_width()||p.y>=dst.get_height()) return;"
            " constexpr sampler s(coord::normalized,address::clamp_to_edge,filter::linear);"
            " float2 uv=(float2(p)+0.5)/float2(dst.get_width(),dst.get_height()); dst.write(src.sample(s,uv,level(0)),p); }";
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"scale_movie"] : nil;
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&error] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2;
        table = [g_dev newArgumentTableWithDescriptor:ad error:&error];
        if (!ps || !table) { MLOG("[oot-movie] diagnostic preparation failed: %s", error.localizedDescription.UTF8String); return 0; }
    }
    rp_end(); blit_close();
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);
    if (!ce) return 0;
    [ce setArgumentTable:table]; [table setTexture:in.gpuResourceID atIndex:0]; [table setTexture:out.gpuResourceID atIndex:1];
    enc_c_pso(ce, ps); enc_c_dispatch(ce, MTLSizeMake((out.width + 7) / 8, (out.height + 7) / 8, 1), MTLSizeMake(8, 8, 1)); enc_c_end(ce);
    args_dirty_all(); return 1;
}
static void oot_movie_remember(NvShader *cs) {
    g_oot_movie_decoded = oot_movie_image(cs, 0, NULL, 0);
    if (g_oot_movie_decoded.pixelFormat != MTLPixelFormatRGBA8Unorm) g_oot_movie_decoded = nil;
}
static const char *const oot_movie_network[] = {
    "ba079f68ae288dbabe8eeaae96b3a4847c520f652a9e282c58c7662f4a8cefed",
    "a8b4fb48fee0ccf87d23bd3dc0e92eca70e3fe11a3226c60de0c0846930cf30a",
    "e2eebd7ba18fe35f0124f8ae33abc500260eeb514d01793fad49933bfb9f412b",
    "e6be82d835fbe4d32e6d77fa7e653fdd80a7b5fa13049ff93e44db5fc93c163e",
    "9442573ee4ace011f83321aaa40b4ded45cc5a07e267fa555de38ee70115c040",
    "0a56002cdc7e1fcee7ee193bf042d2846ebccd24ca72457438c0c712c978943e",
    "0a56002cdc7e1fcee7ee193bf042d2846ebccd24ca72457438c0c712c978943e",
    "d15370a382630d4e7f27fa0eec59333cfa0b2751b7472b3436abbe58d4d9edce",
    "e6be82d835fbe4d32e6d77fa7e653fdd80a7b5fa13049ff93e44db5fc93c163e",
    "7f7ff0f36b54d90f71b63124eea41818cb9ffd387812d1e046d7d19a4b9230cc",
    "a8b4fb48fee0ccf87d23bd3dc0e92eca70e3fe11a3226c60de0c0846930cf30a",
    "5a7d69b067a7f011a4817b4357cffc2f50a2af35ea5c79bb3826aa722c431d11",
    oot_movie_final_key,
};

/* Match the whole same-list sequence before changing GPU work. State and
   barriers still replay normally through mtl_exec. */
static const uint8_t *oot_movie_try_network(const uint8_t *p) {
    if (!p || !g_oot_movie_decoded) return NULL;
    NvShader *cs = nil;
    uint64_t dst = S.img[5][1];
    unsigned index = 0;
    for (unsigned count = 0; count < 128; count++) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < sizeof h || (h.size & 7)) return NULL;
        const void *a = p + sizeof h;
        switch (h.op) {
        case OP_BIND_PROGRAM: {
            const p_bind_program *b = a;
            if (h.size < sizeof h + sizeof *b || b->stages != NVN_SHADER_STAGE_COMPUTE_BIT || !b->prog)
                return NULL;
            NvProg *program = prog_get(b->prog);
            cs = program ? program->sh[5] : nil;
            break;
        }
        case OP_BIND_IMG: {
            const p_bind_handles *b = a;
            if (h.size < sizeof h + offsetof(p_bind_handles, h) || b->stage != 5 || b->src || b->first < 0 ||
                b->first >= 32 || b->n <= 0 || b->n > 32 - b->first ||
                h.size < sizeof h + offsetof(p_bind_handles, h) + sizeof b->h[0] * (unsigned)b->n)
                return NULL;
            if (b->first <= 1 && b->first + b->n > 1) dst = b->h[1 - b->first];
            break;
        }
        case OP_BIND_SSBO: case OP_BIND_UBO: case OP_BIND_TEX: case OP_BIND_SAMP:
        case OP_PUSH_CONST: case OP_BARRIER: break;
        case OP_DISPATCH: {
            const p_dispatch *d = a;
            if (h.size < sizeof h + sizeof *d || d->x <= 0 || d->y <= 0 || d->z != 1 ||
                d->indirect || !cs || !oot_movie_key_is(cs->key, oot_movie_network[index]))
                return NULL;
            if (++index != sizeof oot_movie_network / sizeof *oot_movie_network) break;
            rp_end(); blit_close();
            uint64_t serial = 0;
            id<MTLTexture> out = oot_movie_image_handle(cs, 1, dst, &serial, 1);
            if (!oot_movie_scale(g_oot_movie_decoded, out)) return NULL;
            gpu_owned_mark(serial); zskip_del(serial);
            id<MTLTexture> in = g_oot_movie_decoded;
            atomic_store_explicit(&g_oot_movie_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), memory_order_relaxed);
            static unsigned replaced;
            if (++replaced <= 3 || replaced % 600 == 0)
                MLOG("[oot-movie] direct scale %lux%lu -> %lux%lu; replaced %u matched sequences",
                     (unsigned long)in.width, (unsigned long)in.height, (unsigned long)out.width, (unsigned long)out.height, replaced);
            return p;
        }
        default: return NULL;
        }
        p += h.size;
    }
    return NULL;
}
