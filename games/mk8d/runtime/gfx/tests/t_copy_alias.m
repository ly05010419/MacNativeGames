// Copies through distinct Metal buffers and sparse guest aliases must see earlier GPU writes.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static void finish(void) {
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull), "GPU submission completed");
    CHECK(!mtl_last_commit_error(), "GPU submission has no error");
}
static void bytes(const void *got, const void *want, size_t n, const char *label) {
    CHECK(!memcmp(got, want, n), label);
}
static void separate_buffers(void) {
    size_t pg = getpagesize();
    void *mem = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "no-copy backing allocated"); if (mem == MAP_FAILED) abort();
    id<MTLBuffer> a = [g_dev newBufferWithBytesNoCopy:mem length:pg options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> b = [g_dev newBufferWithBytesNoCopy:mem length:pg options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> io = [g_dev newBufferWithLength:pg options:MTLResourceStorageModeShared];
    CHECK(a && b && io && a.gpuAddress != b.gpuAddress, "one allocation has distinct GPU addresses");
    if (!a || !b || !io) abort(); res_add(a); res_add(b); res_add(io);
    for (int split = 0; split < 2; split++) {
        memset(mem, 0, pg); memset(io.contents, 0, pg); memset(io.contents, 0x6d, 256);
        mtl_sub_begin();
        enc_b_copy_buffer(blit_buf(0x10000, 0x10100, (uint64_t)mem, (uint64_t)mem + 256), io, 0, a, 0, 256);
        if (split) { enc_b_end(g_blit); g_blit = nil; }
        enc_b_copy_buffer(blit_buf((uint64_t)mem, (uint64_t)mem + 256, 0x20000, 0x20100), b, 0, io, 512, 256);
        finish(); bytes((uint8_t *)io.contents + 512, io.contents, 256, "copy reads through a second no-copy buffer");
    }
    res_remove(a); res_remove(b); res_remove(io); a = b = nil; munmap(mem, pg);
}
static void sparse_copies(void) {
    const size_t n = 256, len = 4 * getpagesize();
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    uint8_t *io = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED && io != MAP_FAILED, "sparse test storage allocated");
    if (mem == MAP_FAILED || io == MAP_FAILED) abort();
    pool_t physical, sparse;
    pool_builder_t pb = {.mem=mem, .size=len, .flags=0x24};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&physical, (const NVNmemoryPoolBuilder *)&pb), "physical pool initialized");
    pb = (pool_builder_t){.size=DM_GAME_APP_POOL + 2 * len, .flags=0x200};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&sparse, (const NVNmemoryPoolBuilder *)&pb), "sparse pool initialized");
    map_req maps[] = {{.phys=(NVNmemoryPool *)&physical, .size=len}, {.phys=(NVNmemoryPool *)&physical, .voff=len, .size=len}};
    CHECK(h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sparse, 2, (const NVNmappingRequest *)maps), "two sparse aliases mapped");
    mtl_pool_add(io, len);
    CHECK(sparse.mem != mem, "sparse pool has a distinct guest address");
    mem[0] = 0x51; CHECK(sparse.mem[0] == 0x51 && sparse.mem[len] == 0x51, "sparse mappings share physical bytes");
    // Both dependency directions and two virtual aliases; separate encoders also
    // exercise the producer and consumer barriers at encoder boundaries.
    for (int split = 0; split < 2; split++) for (int direction = 0; direction < 3; direction++) for (int op = 0; op < 4; op++) {
        memset(mem, 0, len); memset(io, 0, len);
        uint32_t value = op == 3 ? 0x78563412 : 0x6d6d6d6d;
        for (size_t i = 0; i < n; i += 4) memcpy(io + i, &value, 4);
        uint64_t dst = (uint64_t)(direction == 0 ? mem : sparse.mem);
        uint64_t src = (uint64_t)(direction == 1 ? mem : sparse.mem + (direction == 2 ? len : 0));
        mtl_sub_begin();
        if (op == 0) mtl_sub_copy((uint64_t)io, dst, n);
        else if (op == 1) mtl_sub_update(io, dst, n);
        else mtl_sub_fill(dst, n, value);
        if (split) ubd_land_compute();
        if (split && g_blit) { enc_b_end(g_blit); g_blit = nil; }
        uint64_t before = mtl_blit_barriers();
        mtl_sub_copy(src, (uint64_t)io + 512, n);
        if (!split) CHECK(mtl_blit_barriers() > before, "sparse alias dependency inserts an intra-encoder barrier");
        finish(); bytes(io + 512, io, n, "sparse alias consumer reads the preceding copy/update/fill");
    }
    // Ordinary non-overlapping buffers keep their existing batching.
    mtl_sub_begin(); uint64_t before = mtl_blit_barriers();
    mtl_sub_copy((uint64_t)io, (uint64_t)mem, n);
    mtl_sub_copy((uint64_t)io + 1024, (uint64_t)mem + 1024, n);
    CHECK(mtl_blit_barriers() == before, "independent ordinary copies need no extra barrier"); finish();
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&sparse); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&physical); mtl_pool_del(io);
    defer_drain(); munmap(mem, len); munmap(io, len);
}
int main(void) { @autoreleasepool {
    mtl_init(); separate_buffers(); sparse_copies();
    printf("t_copy_alias: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
