// Issue #941: the last live indirect draw paints the target. The count is written on the GPU after recording.
#import <Foundation/Foundation.h>
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) {
    (void)p;
    return NULL;
}
module_t *g_mod;
int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

enum { N = 8192, STRIDE = 32, PIXELS = 8 };
static _Alignas(16) uint8_t storage[128 * 1024];
static _Alignas(16) uint8_t parent_storage[256];

static void begin(NVNcommandBuffer *cb, uint8_t *data, size_t size) {
    CB(cb)->cur = data;
    CB(cb)->end = data + size;
    h_nvnCommandBufferBeginRecording(cb);
}

static int run(prog_t *pg, uint8_t *pool, int indexed, int maximum, int count, int lit_at, int stride) {
    // The only nonempty argument record covers the target. Padding also checks that replay uses the supplied stride.
    uint32_t *ind = (void *)(pool + 256);
    uint64_t base = (uint64_t)(uintptr_t)pool, arguments = base + 256;
    uint32_t *indices = (void *)(pool + 64);
    indices[0] = 0;
    indices[1] = 1;
    indices[2] = 2;
    memset(ind, 0, N * STRIDE);
    if (stride < 0)
        arguments += (uint64_t)(maximum - 1) * STRIDE;
    uint32_t *sentinel = (void *)(uintptr_t)(arguments + (uint64_t)lit_at * (uint64_t)stride);
    sentinel[1] = 1;

    NVNcommandBuffer cb = {0}, parent = {0};
    begin(&cb, storage, sizeof storage);
    if (indexed) {
        h_nvnCommandBufferMultiDrawElementsIndirectCount(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_INT, base + 64,
                                                         arguments, base, maximum, stride);
    } else {
        h_nvnCommandBufferMultiDrawArraysIndirectCount(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, arguments, base, maximum, stride);
    }
    NVNcommandHandle draws = h_nvnCommandBufferEndRecording(&cb);
    begin(&parent, parent_storage, sizeof parent_storage);
    h_nvnCommandBufferCopyBufferToBuffer(&parent, base + 16, base, sizeof(uint32_t), 0);
    h_nvnCommandBufferCallCommands(&parent, 1, &draws);
    // Re-enter the same range after an inline GPU write to its last live argument.
    // Its previous masked copy had zero vertices and must not be reused.
    uint32_t vertices = 3;
    h_nvnCommandBufferUpdateUniformBuffer(&parent, (uint64_t)(uintptr_t)sentinel, sizeof vertices, 0, sizeof vertices, &vertices);
    h_nvnCommandBufferCallCommands(&parent, 1, &draws);
    uint64_t handle = h_nvnCommandBufferEndRecording(&parent);

    tex_t rt = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    rt.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D,
                         .format = NVN_FORMAT_RGBA8,
                         .width = PIXELS,
                         .height = PIXELS,
                         .depth = 1,
                         .levels = 1,
                         .samples = 1};
    MTLTextureDescriptor *td = tex_desc(&rt, 0);
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> out = [g_dev newTextureWithDescriptor:td];
    res_add(out);
    mtl_tex_init(&rt);
    serial_put(rt.serial, out);

    ptrdiff_t bytes = CB(&cb)->cur - storage;
    int good = maximum <= 1024 || bytes <= 128;
    // Reuse the same handle with two different GPU-produced counts. A count captured during recording, or
    // a mask reused from a previous submission, can light the target in the wrong submission.
    const int counts[] = {0, count};
    for (size_t pass = 0; pass < sizeof counts / sizeof *counts; pass++) {
        int live = counts[pass];
        sentinel[0] = 0;
        *(uint32_t *)(pool + 16) = (uint32_t)live;
        *(uint32_t *)pool = ~((uint32_t)live);
        uint8_t pixels[PIXELS * PIXELS * 4] = {0};
        [out replaceRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0 withBytes:pixels bytesPerRow:PIXELS * 4];
        state_reset();
        memset(&R, 0, sizeof R);
        mtl_sub_begin();
        mtl_sub_index(&handle, 1);
        R.has_rt = 1;
        R.rt.n = 1;
        R.rt.color[0] = &rt;
        S.prog = pg;
        S.ds.dt = S.ds.dw = S.ds.st = 0;
        S.poly.cull = 0;
        S.color.blend_en = 0;
        S.chmask.mask = 0xffff;
        uint64_t waits = g_mdi_waits;
        mtl_exec((const uint8_t *)(uintptr_t)handle);
        uint64_t fence = mtl_sub_commit();
        int done = mtl_wait(fence, 10000000000ull) && !mtl_last_commit_error();
        if (!done) {
            good = 0;
            break;
        }
        [out getBytes:pixels bytesPerRow:PIXELS * 4 fromRegion:MTLRegionMake2D(0, 0, PIXELS, PIXELS) mipmapLevel:0];
        unsigned red = pixels[((PIXELS / 2) * PIXELS + PIXELS / 2) * 4];
        unsigned expected = lit_at < maximum && lit_at < live ? 255 : 0;
        good &= red == expected && *(uint32_t *)pool == (uint32_t)live;
        if (maximum <= N && stride != 0)
            good &= g_mdi_waits == waits;
        printf("%s max=%d live=%d stride=%d sentinel=%d pixel=%u expected=%u recorded=%td waits=%llu\n",
               indexed ? "indexed" : "arrays", maximum, live, stride, lit_at, red, expected, bytes,
               (unsigned long long)(g_mdi_waits - waits));
    }
    mtl_tex_release(&rt);
    return good;
}
int main(void) {
    @autoreleasepool {
        mtl_init();
        NSError *err = nil;
        id<MTLLibrary> lib =
            [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                         "vertex float4 v(uint i [[vertex_id]]){const float2 "
                                         "p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[i%3],0,1);}\n"
                                         "fragment float4 f(){return float4(1,0,0,1);}"
                                options:nil
                                  error:&err];
        if (!lib) {
            fprintf(stderr, "%s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        size_t bytes = 512 * 1024;
        uint8_t *pool = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (pool == MAP_FAILED)
            return 2;
        NVNmemoryPool physical, virt;
        pool_builder_t builder = {.mem = pool, .size = bytes, .flags = 0x24};
        if (!h_nvnMemoryPoolInitialize(&physical, (const NVNmemoryPoolBuilder *)&builder))
            return 2;
        // Monster Hunter Rise keeps both its count and its strided argument records in a sparse virtual pool.
        builder = (pool_builder_t){.size = DM_GAME_APP_POOL + 4 * bytes, .flags = NVN_MEMORY_POOL_FLAGS_VIRTUAL};
        if (!h_nvnMemoryPoolInitialize(&virt, (const NVNmemoryPoolBuilder *)&builder))
            return 2;
        map_req mapping = {.phys = &physical, .size = bytes};
        if (!h_nvnMemoryPoolMapVirtual(&virt, 1, (const NVNmappingRequest *)&mapping) || !nvn_sparse_pool(&virt))
            return 2;
        uint8_t *alias = (uint8_t *)(uintptr_t)h_nvnMemoryPoolGetBufferAddress(&virt);
        NvProg *pr = [NvProg new];
        pr->sh[0] = [NvShader new];
        pr->sh[1] = [NvShader new];
        pr->sh[0]->fn = [lib newFunctionWithName:@"v"];
        pr->sh[1]->fn = [lib newFunctionWithName:@"f"];
        pr->sh[1]->stage = 1;
        pr->sh[1]->rf.color_known = 1;
        pr->sh[1]->rf.color_mask = 1;
        prog_t pg = {.serial = nvn_new_serial()};
        serial_put(pg.serial, pr);
        int failures = 0; // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_put retains pr in g_by_serial and serial_del releases it after the draws.
        for (int indexed = 0; indexed <= 1; indexed++) {
            failures += !run(&pg, pool, indexed, 1024, 1024, 1023, STRIDE);
            failures += !run(&pg, pool, indexed, 1025, 1025, 1024, STRIDE);
            failures += !run(&pg, pool, indexed, 4594, 4593, 4593, STRIDE);
            failures += !run(&pg, pool, indexed, 4594, 8192, 4594, STRIDE);
            failures += !run(&pg, pool, indexed, 8192, 8192, 8191, STRIDE);
            failures += !run(&pg, pool, indexed, 4594, 4594, 4593, -STRIDE);
            failures += !run(&pg, pool, indexed, 1025, 1, 0, 0);
            failures += !run(&pg, pool, indexed, INT_MAX, 2, 1, STRIDE);
        }
        failures += !run(&pg, alias, 0, 4594, 4594, 4593, STRIDE);
        failures += !run(&pg, alias, 1, 8192, 8192, 8191, STRIDE);
        h_nvnMemoryPoolFinalize(&virt);
        h_nvnMemoryPoolFinalize(&physical);
        serial_del(pg.serial);
        mtl_drain();
        munmap(pool, bytes);
        return failures != 0;
    }
}
