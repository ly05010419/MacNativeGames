// Real Metal readback: cached stream bindings survive helper overwrites, encoder/submit changes and pool replacement.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }   // Tests use host-thread stacks.
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)

static id<MTLRenderPipelineState> pipeline(void) {
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex void v(const device uint *a[[buffer(16)]], const device uint *b[[buffer(14)]],"
        "const device uint *c[[buffer(15)]], const device uint *z[[buffer(30)]], device uint4 *o[[buffer(8)]])"
        "{ *o=uint4(*a,*b,*c,*z); }" options:nil error:&err];
    if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); abort(); }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"v"]; d.rasterizationEnabled = NO;
    id<MTLRenderPipelineState> ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); abort(); }
    return ps;
}
static id<MTL4RenderCommandEncoder> encoder(id<MTLRenderPipelineState> ps) {
    MTL4RenderPassDescriptor *d = [MTL4RenderPassDescriptor new];
    d.renderTargetWidth = d.renderTargetHeight = d.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(d); enc_r_pso(en, ps); return en;
}
static void capture(id<MTL4RenderCommandEncoder> en, id<MTLBuffer> out, unsigned row) {
    enc_r_vbuf(en, TF_SLOT, out.gpuAddress + row * 16); enc_r_draw(en, MTLPrimitiveTypePoint, 0, 1);
}
static void finish(void) {
    EXPECT("submission completed", 1, mtl_wait(mtl_sub_commit(), 5000000000ull));
    EXPECT("no GPU error", 0, mtl_last_commit_error());
}
int main(void) { @autoreleasepool {
    mtl_init(); id<MTLRenderPipelineState> ps = pipeline();
    size_t size = (size_t)getpagesize(); uint32_t *mem;
    if (posix_memalign((void **)&mem, size, size)) abort();
    memset(mem, 0, size); mem[0] = 11; mem[1] = 22; mem[2] = 33; mem[3] = 99;
    mtl_pool_add(mem, size);
    id<MTLBuffer> out = [g_dev newBufferWithLength:16 * 8 options:MTLResourceStorageModeShared]; res_add(out);
    state_reset(); S.vbuf[0].address = (uint64_t)mem; S.vbuf[14].address = (uint64_t)(mem + 1); S.vbuf[15].address = (uint64_t)(mem + 2);
    uint64_t other, off; void *mb; mtl_gpu_addr((uint64_t)(mem + 3), 1, &mb, &off, &other);
    mtl_sub_begin(); id<MTL4RenderCommandEncoder> en = encoder(ps);
    bind_vbufs(en); capture(en, out, 0);
    uint64_t resolves = g_vbuf_resolve, sets = g_vbuf_set;
    for (int i = 0; i < 1000; i++) bind_vbufs(en);
    EXPECT("unchanged streams avoid lookups", g_vbuf_cache ? 0 : 3000, g_vbuf_resolve - resolves);
    EXPECT("unchanged table avoids writes", g_vbuf_cache ? 0 : 4000, g_vbuf_set - sets);
    capture(en, out, 1);
    // A converted stream changes the real table, even though the guest stream address is unchanged.
    enc_r_vbuf(en, VB_SLOT(0), other); capture(en, out, 2);
    bind_vbufs(en); capture(en, out, 3);
    // All vertex table writers must participate, including argument-buffer writes.
    enc_r_args(en, MTLRenderStageVertex, other, VB_SLOT(14)); bind_vbufs(en); capture(en, out, 4);
    enc_r_end(en);
    en = encoder(ps); bind_vbufs(en); capture(en, out, 5); enc_r_end(en); finish();
    mtl_sub_begin(); en = encoder(ps);
    S.vbuf[0].address = (uint64_t)(mem + 3); bind_vbufs(en); capture(en, out, 6); enc_r_end(en); finish();
    // The same guest address can acquire a new Metal GPU address. Hold the old buffer to prevent address recycling.
    id<MTLBuffer> old = (__bridge id<MTLBuffer>)mb;
    mtl_pool_del(mem); mtl_pool_add(mem, size);
    uint64_t newgpu; mtl_gpu_addr((uint64_t)(mem + 3), 1, &mb, &off, &newgpu);
    EXPECT("replacement buffer has a different GPU address", 1, old.gpuAddress + 12 != newgpu);
    mtl_sub_begin(); en = encoder(ps); resolves = g_vbuf_resolve;
    bind_vbufs(en); EXPECT("pool replacement resolves all streams", 3, g_vbuf_resolve - resolves);
    EXPECT("replacement address bound", newgpu, g_vt_addr[VB_SLOT(0)]);
    capture(en, out, 7); enc_r_end(en); finish();
    const uint32_t *pixels = out.contents;
    for (int row = 0; row < 8; row++) {
        EXPECT("stream 0 readback", row == 2 || row >= 6 ? 99 : 11, pixels[row * 4]);
        EXPECT("stream 14 readback", 22, pixels[row * 4 + 1]);
        EXPECT("stream 15 readback", 33, pixels[row * 4 + 2]);
        EXPECT("zero buffer readback", 0, pixels[row * 4 + 3]);
    }
    // CPU-only binding microbenchmark, after GPU validation; reports overhead, not game frame time.
    for (int mode = 0; mode < 2; mode++) {
        g_vbuf_cache = mode; bind_vbufs(nil); uint64_t start = bp_now(), r = g_vbuf_resolve, w = g_vbuf_set;
        for (int i = 0; i < 200000; i++) bind_vbufs(nil);
        printf("bindings cache=%d: %.2f ns/draw, lookups=%llu writes=%llu\n", mode, (bp_now() - start) / 200000.0,
            (unsigned long long)(g_vbuf_resolve - r), (unsigned long long)(g_vbuf_set - w));
    }
    mtl_pool_del(mem); mtl_drain(); old = nil; free(mem);
    printf("t_vbuf: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
