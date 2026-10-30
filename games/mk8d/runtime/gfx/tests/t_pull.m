// The pulling vertex fetch (dm/gfx/tools/dmsh_pull.metal) against Metal's own: for every NVN vertex format the host knows (nvn_mtl.m vfmt), the words dm_fetch gives must be bit
// for bit the ones Metal's vertex-descriptor fetch gives an input of the format's class (float4, uint4 or int4); for the formats Metal has no fetch for (integers read as floats,
// vconv_i2f), the ones the host's CPU conversion gives (vconv_fill). Every byte value, half and float specials, snorm -128, 10:10:10:2 extremes, 8-bit attributes at aligned and
// at 2-byte offsets, a stride-1 stream; then stride 0 and a divisor with a base instance, which must pick the elements Metal's step functions pick; then the host's layout block
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

typedef struct { uint64_t base; uint32_t stride, divisor; } dm_vstream_c;
typedef struct { dm_vstream_c s[16]; uint32_t attr[16]; } dm_vlayout_c;   // dmsh_pull.metal dm_vlayout
_Static_assert(sizeof(dm_vlayout_c) == 320, "dm_vlayout");
static id<MTLCommandQueue> q; static id<MTLTexture> rt; static id<MTLLibrary> lib;
static NSString *k_test = @"\n"
"vertex void pull_test(constant dm_vlayout &L [[buffer(16)]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]], uint bi [[base_instance]]) { out[mode ? iid : vid] = dm_fetch(L, 0, vid, iid, bi); }\n"
"struct vinf { float4 a [[attribute(0)]]; }; struct vinu { uint4 a [[attribute(0)]]; }; struct vini { int4 a [[attribute(0)]]; };\n"
"vertex void vd_f(vinf in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = as_type<uint4>(in.a); }\n"
"vertex void vd_u(vinu in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = in.a; }\n"
"vertex void vd_i(vini in [[stage_in]], device uint4 *out [[buffer(1)]], constant uint &mode [[buffer(2)]], uint vid [[vertex_id]], uint iid [[instance_id]]) { out[mode ? iid : vid] = as_type<uint4>(in.a); }\n";

static id<MTLRenderPipelineState> rp(NSString *fn, MTLVertexDescriptor *vd) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [lib newFunctionWithName:fn]; d.rasterizationEnabled = NO; d.vertexDescriptor = vd;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *e = nil; id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { fprintf(stderr, "pipeline %s: %s\n", fn.UTF8String, e.localizedDescription.UTF8String); abort(); }
    return ps;
}
static void run(id<MTLRenderPipelineState> ps, id<MTLBuffer> vb, int vd_slot, id<MTLBuffer> lay, id<MTLBuffer> out, uint32_t mode, int nvert, int ninst, int binst) {
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor new]; p.colorAttachments[0].texture = rt; p.colorAttachments[0].loadAction = MTLLoadActionDontCare; p.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:ps];
    if (vd_slot >= 0) [e setVertexBuffer:vb offset:0 atIndex:(NSUInteger)vd_slot];
    if (lay) { [e setVertexBuffer:lay offset:0 atIndex:16]; [e useResource:vb usage:MTLResourceUsageRead stages:MTLRenderStageVertex]; }
    [e setVertexBuffer:out offset:0 atIndex:1]; [e setVertexBytes:&mode length:4 atIndex:2];
    [e drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:(NSUInteger)nvert instanceCount:(NSUInteger)ninst baseInstance:(NSUInteger)binst];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("command buffer without error", 0, cb.error != nil);
}
static int int_class(MTLVertexFormat f) {   // 1 unsigned, 2 signed integer input; 0 float
    switch (f) {
        case MTLVertexFormatUChar: case MTLVertexFormatUChar2: case MTLVertexFormatUChar3: case MTLVertexFormatUChar4: case MTLVertexFormatUShort: case MTLVertexFormatUShort2:
        case MTLVertexFormatUShort3: case MTLVertexFormatUShort4: case MTLVertexFormatUInt: case MTLVertexFormatUInt2: case MTLVertexFormatUInt3: case MTLVertexFormatUInt4: return 1;
        case MTLVertexFormatChar: case MTLVertexFormatChar2: case MTLVertexFormatChar3: case MTLVertexFormatChar4: case MTLVertexFormatShort: case MTLVertexFormatShort2:
        case MTLVertexFormatShort3: case MTLVertexFormatShort4: case MTLVertexFormatInt: case MTLVertexFormatInt2: case MTLVertexFormatInt3: case MTLVertexFormatInt4: return 2;
        default: return 0;
    }
}
static int elem_size(int f) {   // bytes of one element of NVN format f
    int ib, is, in;
    if ((in = vconv_i2f(f, &ib, &is)) || (in = vconv_int(f, &ib, &is))) return in * ib / 8;
    switch (vfmt(f)) {
        case MTLVertexFormatUCharNormalized: case MTLVertexFormatCharNormalized: return 1;
        case MTLVertexFormatHalf: case MTLVertexFormatUShortNormalized: case MTLVertexFormatShortNormalized: case MTLVertexFormatUChar2Normalized: case MTLVertexFormatChar2Normalized: return 2;
        case MTLVertexFormatUChar3Normalized: case MTLVertexFormatChar3Normalized: return 3;
        case MTLVertexFormatHalf3: case MTLVertexFormatUShort3Normalized: case MTLVertexFormatShort3Normalized: return 6;
        case MTLVertexFormatHalf4: case MTLVertexFormatUShort4Normalized: case MTLVertexFormatShort4Normalized: case MTLVertexFormatFloat2: case MTLVertexFormatUInt2: case MTLVertexFormatInt2: return 8;
        case MTLVertexFormatFloat3: case MTLVertexFormatUInt3: case MTLVertexFormatInt3: return 12;
        case MTLVertexFormatFloat4: case MTLVertexFormatUInt4: case MTLVertexFormatInt4: return 16;
        default: return 4;
    }
}
int main(void) { @autoreleasepool {
    mtl_init(); q = [g_dev newCommandQueue]; NSError *err = nil;
    NSString *fetch = [NSString stringWithContentsOfFile:@"../tools/dmsh_pull.metal" encoding:NSUTF8StringEncoding error:&err];
    if (!fetch) { fprintf(stderr, "dmsh_pull.metal: %s\n", err.localizedDescription.UTF8String); return 1; }
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES;   // as the chain compiles (fast math on)
    lib = [g_dev newLibraryWithSource:[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n%@\n%@", fetch, k_test] options:o error:&err];
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; rt = [g_dev newTextureWithDescriptor:td];
    enum { NV = 512 };
    id<MTLBuffer> out = [g_dev newBufferWithLength:NV * 16 options:MTLResourceStorageModeShared], lay = [g_dev newBufferWithLength:sizeof(dm_vlayout_c) options:MTLResourceStorageModeShared];
    id<MTLRenderPipelineState> pull = rp(@"pull_test", nil);
    srand(12345);
    int nfmt = 0, nvd = 0, ncpu = 0;
    for (int f = 1; f < 256; f++) {
        int ib, is, i2f = vconv_i2f(f, &ib, &is) != 0;
        MTLVertexFormat mf = vfmt(f);
        if (mf == MTLVertexFormatInvalid) continue;   // a format the host does not know: no fetch either way
        nfmt++;
        int es = elem_size(f), ic = int_class(mf);
        int k8 = !i2f && (mf == MTLVertexFormatUCharNormalized || mf == MTLVertexFormatCharNormalized || mf == MTLVertexFormatUChar || mf == MTLVertexFormatChar || mf == MTLVertexFormatUChar2Normalized ||
                 mf == MTLVertexFormatChar2Normalized || mf == MTLVertexFormatUChar2 || mf == MTLVertexFormatChar2 || mf == MTLVertexFormatUChar3Normalized || mf == MTLVertexFormatChar3Normalized ||
                 mf == MTLVertexFormatUChar3 || mf == MTLVertexFormatChar3 || mf == MTLVertexFormatUChar4Normalized || mf == MTLVertexFormatChar4Normalized || mf == MTLVertexFormatUChar4 ||
                 mf == MTLVertexFormatChar4 || mf == MTLVertexFormatUChar4Normalized_BGRA);
        for (int pass = 0; pass < (k8 ? 3 : 1); pass++) {   // 8-bit formats: 4-byte aligned, at a 2-byte offset, and (1-byte ones) at stride 1
            int stride = ((es + 3) & ~3) + 4, off = 4;
            if (pass == 1) off = 2;
            if (pass == 2) { if (es != 1) continue; stride = 1; off = 0; }
            id<MTLBuffer> vb = [g_dev newBufferWithLength:(NSUInteger)(NV * stride + 64) options:MTLResourceStorageModeShared];
            uint8_t *p = vb.contents; for (int i = 0; i < NV * stride + 64; i++) p[i] = (uint8_t)rand();
            for (int v = 0; v < NV && stride > 1; v++) {   // edge values
                uint8_t *e = p + v * stride + off;
                if (k8) for (int c = 0; c < es; c++) e[c] = (uint8_t)(v + 37 * c);   // every byte value in each component, -128 included
                if ((mf == MTLVertexFormatHalf || mf == MTLVertexFormatHalf2 || mf == MTLVertexFormatHalf3 || mf == MTLVertexFormatHalf4) && v < 16) {
                    static const uint16_t H[16] = { 0x0000, 0x8000, 0x0001, 0x03ff, 0x0400, 0x7bff, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xfe00, 0x3c00, 0xbc00, 0x8001, 0x7fff, 0xffff };
                    for (int c = 0; c < es / 2; c++) memcpy(e + 2 * c, &H[(v + c) & 15], 2); }
                if ((mf == MTLVertexFormatFloat || mf == MTLVertexFormatFloat2 || mf == MTLVertexFormatFloat3 || mf == MTLVertexFormatFloat4) && v < 8) {
                    static const uint32_t F[8] = { 0x7fc12345, 0x7f800001, 0x00000001, 0x80000000, 0x7f800000, 0xff800000, 0x807fffff, 0x3f800000 };
                    for (int c = 0; c < es / 4; c++) memcpy(e + 4 * c, &F[(v + c) & 7], 4); }
                if ((mf == MTLVertexFormatInt1010102Normalized || mf == MTLVertexFormatUInt1010102Normalized) && v < 4) {
                    uint32_t w = v == 0 ? 0x80000000u | 0x200u | (0x200u << 10) | (0x200u << 20) : v == 1 ? 0x40000000u | 0x1ffu : v == 2 ? 0xc0000000u : 0x3fffffffu; memcpy(e, &w, 4); }
            }
            dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = (uint32_t)stride; L.attr[0] = (uint32_t)f | (uint32_t)off << 8; memcpy(lay.contents, &L, sizeof L);
            uint32_t got[NV][4], want[NV][4];
            memset(out.contents, 0xcd, NV * 16); run(pull, vb, -1, lay, out, 0, NV, 1, 0); memcpy(got, out.contents, sizeof got);
            if (i2f) {   // Metal has no such fetch: the host converts on the CPU, and the pulled words must be those
                float conv[NV][4]; vconv_fill(f, (uint64_t)(uintptr_t)(p + off), (uint32_t)stride, NV, &conv[0][0]); memcpy(want, conv, sizeof want); ncpu++;
            } else {
                MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor]; vd.attributes[0].format = mf; vd.attributes[0].offset = (NSUInteger)off; vd.attributes[0].bufferIndex = 20;
                vd.layouts[20].stride = (NSUInteger)stride; vd.layouts[20].stepFunction = MTLVertexStepFunctionPerVertex;
                memset(out.contents, 0xcd, NV * 16); run(rp(ic == 1 ? @"vd_u" : ic == 2 ? @"vd_i" : @"vd_f", vd), vb, 20, nil, out, 0, NV, 1, 0); memcpy(want, out.contents, sizeof want); nvd++;
            }
            int bad = 0, ex = 0;
            for (int v = 0; v < NV; v++) for (int c = 0; c < 4; c++) if (got[v][c] != want[v][c]) { bad++; if (ex++ < 3) fprintf(stderr, "  format %#x stride %d offset %d vertex %d component %d: pulled %08x, %s %08x\n", f, stride, off, v, c, got[v][c], i2f ? "CPU conversion" : "Metal fetch", want[v][c]); }
            char name[96]; snprintf(name, sizeof name, "format %#x (stride %d, offset %d): words differing", f, stride, off);
            EXPECT(name, 0, bad);
        }
    }
    printf("%d formats: %d comparisons with Metal's fetch, %d with the host's CPU conversion\n", nfmt, nvd, ncpu);
    EXPECT("formats vfmt knows (0x01-0x30, 10:10:10:2 both, R11G11B10F, BGRA8, 24 integer-as-float)", 76, nfmt);
    // stride 0 (Metal: constant step) and a divisor with a base instance (Metal: per-instance step rate): the element each instance reads
    id<MTLBuffer> vb = [g_dev newBufferWithLength:64 * 16 options:MTLResourceStorageModeShared];
    float *fp = vb.contents; for (int i = 0; i < 64 * 4; i++) fp[i] = (float)i;
    for (int t = 0; t < 4; t++) {
        int divisor = t, zero = t == 0, binst = 5, ninst = 7;
        dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = zero ? 0 : 16; L.s[0].divisor = (uint32_t)divisor; L.attr[0] = 0x2E; memcpy(lay.contents, &L, sizeof L);
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor]; vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].bufferIndex = 20;
        vd.layouts[20].stride = zero ? 4 : 16; vd.layouts[20].stepFunction = zero ? MTLVertexStepFunctionConstant : MTLVertexStepFunctionPerInstance; vd.layouts[20].stepRate = zero ? 0 : (NSUInteger)divisor;
        uint32_t a[16][4], b[16][4];
        memset(out.contents, 0, NV * 16); run(rp(@"vd_f", vd), vb, 20, nil, out, 1, 1, ninst, binst); memcpy(a, out.contents, sizeof a);
        memset(out.contents, 0, NV * 16); run(pull, vb, -1, lay, out, 1, 1, ninst, binst); memcpy(b, out.contents, sizeof b);
        for (int i = binst; i < binst + ninst; i++) {
            char name[96]; snprintf(name, sizeof name, "%s, base instance 5: element instance %d reads", zero ? "stride 0" : divisor == 1 ? "divisor 1" : divisor == 2 ? "divisor 2" : "divisor 3", i);
            EXPECT(name, a[i][0], b[i][0]);
        }
    }
    // the host's layout block (pull_layout_fill): a stream whose base is not 4-byte aligned, its low bits folded into the attribute offsets. Each element must read as the same bytes
    // copied to an aligned place do. Elements keep their components' alignment, as for Metal's fetch (16-bit components at even addresses, packed 32-bit words at multiples of 4;
    // 8-bit ones anywhere); an attribute whose stream has no buffer reads zeros
    {
        size_t pg = (size_t)getpagesize(); uint8_t *mem; if (posix_memalign((void **)&mem, pg, pg)) abort();
        for (size_t i = 0; i < pg; i++) mem[i] = (uint8_t)rand();
        mtl_pool_add(mem, pg);
        enum { ST = 24, NE = 32 };
        int fmts[4] = { 0x29, 0x69, 0x25, 0x0F }, size[4] = { 8, 4, 4, 2 };   // RGBA16F, RGB10A2 snorm, RGBA8 unorm, RG8UI
        void *mb; uint64_t mo, gpu0; mtl_gpu_addr((uint64_t)(uintptr_t)mem, 1, &mb, &mo, &gpu0);
        id<MTLBuffer> one = [g_dev newBufferWithLength:sizeof(dm_vlayout_c) options:MTLResourceStorageModeShared], ref = [g_dev newBufferWithLength:NE * 16 options:MTLResourceStorageModeShared];
        for (int mis = 1; mis < 4; mis++) {
            int offs[4] = { mis & 1, 12 - mis, 13, 17 };   // element addresses base + mis + offset: even, a multiple of 4, odd, odd
            state_reset(); S.nattr = 5; S.vbuf[2].address = (uint64_t)(uintptr_t)(mem + 64 + mis); S.stream[2].stride = ST;
            for (int a = 0; a < 4; a++) { S.attr[a].fmt = (uint32_t)fmts[a]; S.attr[a].offset = (uint32_t)offs[a]; S.attr[a].stream = 2; }
            S.attr[4].fmt = 0x2E; S.attr[4].stream = 5;   // stream 5 has no buffer
            pull_layout P; pull_layout_fill(&P);
            EXPECT("base kept 4-byte aligned", 0, P.s[2].base & 3);
            EXPECT("attribute with no buffer reads zeros", 0, P.attr[4]);
            for (int a = 0; a < 4; a++) {
                uint32_t got[NE][4], want[NE][4];
                dm_vlayout_c L1; memset(&L1, 0, sizeof L1); L1.s[2].base = P.s[2].base; L1.s[2].stride = P.s[2].stride; L1.attr[0] = P.attr[a]; memcpy(one.contents, &L1, sizeof L1);
                memset(out.contents, 0, NV * 16); run(pull, (__bridge id<MTLBuffer>)mb, -1, one, out, 0, NE, 1, 0); memcpy(got, out.contents, sizeof got);
                for (int v = 0; v < NE; v++) memcpy((uint8_t *)ref.contents + v * 16, mem + 64 + mis + v * ST + offs[a], (size_t)size[a]);   // each element at an aligned place
                dm_vlayout_c L2; memset(&L2, 0, sizeof L2); L2.s[2].base = ref.gpuAddress; L2.s[2].stride = 16; L2.attr[0] = (uint32_t)fmts[a] | 2u << 27; memcpy(one.contents, &L2, sizeof L2);
                memset(out.contents, 0, NV * 16); run(pull, ref, -1, one, out, 0, NE, 1, 0); memcpy(want, out.contents, sizeof want);
                char name[96]; snprintf(name, sizeof name, "base +%d, format %#x at offset %d: words as from an aligned copy", mis, fmts[a], offs[a]);
                EXPECT(name, 0, (uint64_t)(memcmp(got, want, sizeof got) != 0));
            }
        }
    }
    printf("t_pull: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
