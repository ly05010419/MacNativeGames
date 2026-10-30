/* Indexed QUADS have no Metal primitive. Expand the actual indices on the same
   GPU timeline, including GPU-written sources. The output stays in the submit
   arena until completion. Rotate each triangle to put the quad's last vertex
   first, preserving NVN's default flat-shading provoking vertex on Metal. */
typedef struct { __unsafe_unretained id<MTLBuffer> indices, args; size_t off, arg_off; uint32_t capacity; } quad_gpu;
static quad_gpu quad_expand_gpu(uint64_t src, uint32_t count, unsigned width,
                               p_primitive_restart restart, int32_t base_vertex,
                               uint32_t instances, uint32_t base_instance) {
    if (width != 1 && width != 2 && width != 4) MDIE("invalid quad index width %u", width);
    static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at;
    if (!ps) {
        NSError *e = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "uint read_index(device const uchar *s, uint at, uint width) { uint v=0; for(uint b=0;b<width;b++) v|=uint(s[at+b])<<(b*8); return v; }\n"
        "void emit_quad(device uint *d, thread uint *q) { d[0]=q[3];d[1]=q[0];d[2]=q[1];d[3]=q[3];d[4]=q[1];d[5]=q[2]; }\n"
        "kernel void quads(device const uchar *s [[buffer(0)]], device uint *d [[buffer(1)]], device uint *args [[buffer(2)]], constant uint *p [[buffer(3)]], uint id [[thread_position_in_grid]]) {"
        " uint count=p[0],width=p[1],shift=p[2],q[4];"
        " if(p[3]) { if(id) return; uint n=0,o=0,mask=width==4?0xffffffffu:(1u<<(width*8))-1u;"
        " for(uint i=0;i<count;i++){ uint v=read_index(s,shift+i*width,width); if(v==(p[4]&mask)){n=0;continue;} q[n++]=v; if(n==4){emit_quad(d+o,q);o+=6;n=0;} } args[0]=o;"
        " } else { if(id<count/4){for(uint i=0;i<4;i++)q[i]=read_index(s,shift+(id*4+i)*width,width);emit_quad(d+id*6,q);} if(id) return; args[0]=(count/4)*6; }"
        " args[1]=p[5];args[2]=0;args[3]=p[6];args[4]=p[7]; }";
        id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&e];
        id<MTLFunction> fn = [lib newFunctionWithName:@"quads"];
        ps = fn ? [g_dev newComputePipelineStateWithFunction:fn error:&e] : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 4;
        at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        if (!ps || !at) MDIE("indexed quad conversion: %s", e.localizedDescription.UTF8String);
    }
    uint64_t base = src & ~15ull; uint32_t shift = (uint32_t)(src - base);
    quad_gpu out = {.capacity = (count / 4) * 6};
    id<MTLBuffer> ib, ab, pb; size_t po;
    out.off = arena_alloc(MAX((size_t)out.capacity * 4, 16u), &ib); out.indices = ib;
    out.arg_off = arena_alloc(32, &ab); out.args = ab; po = arena_alloc(32, &pb);
    uint32_t params[8] = {count, width, shift, restart.enabled, restart.index, instances, (uint32_t)base_vertex, base_instance};
    memcpy((uint8_t *)pb.contents + po, params, sizeof params);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL); [ce setArgumentTable:at];
    [at setAddress:base atIndex:0]; [at setAddress:ib.gpuAddress + out.off atIndex:1];
    [at setAddress:ab.gpuAddress + out.arg_off atIndex:2]; [at setAddress:pb.gpuAddress + po atIndex:3];
    enc_c_pso(ce, ps);
    NSUInteger threads = restart.enabled ? 1 : MAX(count / 4, 1u);
    enc_c_dispatch(ce, MTLSizeMake((threads + 63) / 64, 1, 1), MTLSizeMake(64, 1, 1));
    enc_c_end(ce); args_dirty_all();
    return out;
}
#define QUAD_MAX 4096
static struct { const p_draw_idx *record; p_draw_idx packet; p_primitive_restart restart; quad_gpu gpu; } g_iquads[QUAD_MAX];
static unsigned g_niquads; static uint64_t g_iquad_submit;
static int quad_find(const p_draw_idx *di) {
    if (g_iquad_submit != g_sub_serial) { g_niquads = 0; g_iquad_submit = g_sub_serial; }
    for (unsigned i = g_niquads; i-- > 0;) if (g_iquads[i].record == di && !memcmp(&g_iquads[i].packet, di, sizeof *di) &&
        !memcmp(&g_iquads[i].restart, &S.restart, sizeof S.restart)) return (int)i;
    return -1;
}
static void quad_prepare(const p_draw_idx *di) {
    /* A top-level execution always reconverts: the same packet can run again
       after a GPU write. Nested deferred replay reuses that execution only. */
    int found = quad_find(di);
    if (g_draw_depth > 1 && found >= 0) return;
    if (g_draw_depth > 1 || g_niquads == QUAD_MAX) MDIE("indexed quad conversion missing before deferred replay or table full");
    if (di->itype < 0 || di->itype > 2) MDIE("invalid quad index type %d", di->itype);
    unsigned width = 1u << di->itype; void *mb; uint64_t mo, gpu;
    if (!mtl_gpu_addr(di->ibuf, (uint64_t)di->count * width, &mb, &mo, &gpu)) MDIE("indexed quad input range not in a memory pool");
    quad_gpu out = quad_expand_gpu(gpu, (uint32_t)di->count, width, S.restart, di->base_vertex, di->instances ? (uint32_t)di->instances : 1, (uint32_t)di->base_instance);
    g_iquads[g_niquads++] = (typeof(g_iquads[0])){di, *di, S.restart, out};
}
static void quad_draw(id<MTL4RenderCommandEncoder> en, const p_draw_idx *di) {
    if (di->count < 4) return;
    int at = quad_find(di); if (at < 0) MDIE("indexed quads were not prepared on the GPU timeline");
    quad_gpu q = g_iquads[at].gpu;
    enc_r_draw_indexed_indirect(en, MTLPrimitiveTypeTriangle, MTLIndexTypeUInt32,
        q.indices.gpuAddress + q.off, (size_t)q.capacity * 4, q.args.gpuAddress + q.arg_off);
}
