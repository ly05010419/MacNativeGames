// The set of CPU-written textures (mtl_tex_cpu_written, tex_cpu_written, mtl_tex_release): membership, no duplicates, a released texture leaving it, and lookups of textures
// not in it staying cheap with thousands of consecutive serials in it (a CFSet that hashed serials as numbers probed a whole bucket run per miss: half of Batman: Arkham Knight's
// rendering thread with 6,442 CPU-written textures)
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

static void retired_classification(void) {
    uint8_t mem[2]; pool_t pools[2] = {{.mem=&mem[0]}, {.mem=&mem[1]}};
    tex_t a = {.serial=nvn_new_serial(), .pool=&pools[0], .mem=&mem[0]};
    tex_t b = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_tex_cpu_written(&a); mtl_tex_cpu_written(&b);
    mtl_tex_release(&a);
    CHECK(mtl_cpu_tex_count() == 1 && !nvn_hm_get(&g_cpu_tex,&a.serial), "release removes live membership");
    CHECK(tex_cpu_written(a.serial), "recorded snapshots retain CPU classification");
    uint64_t gen = g_ser_gen; mtl_tex_cpu_written(&a);
    CHECK(mtl_cpu_tex_count() == 1 && g_ser_gen == gen, "late repeated marking cannot resurrect a released texture");
    mtl_alias_drop_pool(&pools[0]);
    CHECK(!tex_cpu_written(a.serial) && tex_cpu_written(b.serial), "pool cleanup preserves the other pool's live membership");
    // A poll may first discover CPU writes after another thread finalized the texture.
    tex_t late = {.serial=nvn_new_serial(), .pool=&pools[1], .mem=&mem[1]};
    mtl_tex_init(&late); mtl_tex_release(&late); gen = g_ser_gen; mtl_tex_cpu_written(&late);
    CHECK(tex_cpu_written(late.serial) && mtl_cpu_tex_count() == 1 && g_ser_gen == gen+1, "first marking after release records only retired classification");
    mtl_tex_release(&b); mtl_tex_release(&b);
    CHECK(mtl_cpu_tex_count() == 0 && tex_cpu_written(b.serial), "repeated release keeps retired snapshots classified");
    mtl_alias_drop_pool(&pools[1]);
    CHECK(!tex_cpu_written(b.serial) && !tex_cpu_written(late.serial) && !g_cpu_tex_retired.n, "pool cleanup removes all retired classifications");
}
static void descriptor_replacement(void) {
    tex_t a = {.serial=nvn_new_serial()}, b = {.serial=nvn_new_serial()};
    mtl_tex_init(&a); mtl_tex_init(&b);
    mtl_reg_tex(1,&a,NULL); mtl_reg_tex(2,&a,NULL); mtl_reg_tex(3,&a,NULL);
    mtl_reg_tex(2,&b,NULL); mtl_reg_tex(3,NULL,NULL);
    mtl_tex_release(&a);
    CHECK(g_treg[1].retired && !g_treg[2].retired && !g_treg[3].ok, "replacing middle and head descriptors preserves serial ownership");
    mtl_reg_tex(1,&b,NULL); mtl_reg_tex(2,NULL,NULL);
    CHECK(!nvn_hm_get(&g_treg_serial,&a.serial), "replacing the last descriptor removes the old serial");
    mtl_tex_release(&b);
    CHECK(g_treg[1].retired, "the replacement texture retires its current descriptors");
    mtl_reg_tex(1,NULL,NULL);
    CHECK(!g_treg_serial.n, "clearing descriptors removes serial links");
}
static void finalized_upload(int write_after_record) {
    const size_t len = 262144;
    uint8_t *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) abort();
    pool_t pool = {0}; pool_builder_t pb = {.mem=mem, .size=len, .flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    CHECK(h_nvnMemoryPoolInitialize((NVNmemoryPool *)&pool, (const NVNmemoryPoolBuilder *)&pb), "CPU-cached pool initialized");
    NVNtextureBuilder tb; NVNtexture tex = {0}; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 4); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (NVNmemoryPool *)&pool, 65536);
    CHECK(h_nvnTextureInitialize(&tex, &tb), "texture without packaged layout initialized");
    uint64_t serial = ((tex_t *)&tex)->serial;
    uint8_t pixels[64]; for (int i=0;i<64;i++) pixels[i]=(uint8_t)(i+32);
    NVNcopyRegion region = {0,0,0,4,4,1};
    if (!write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    mtl_reg_tex(1, (const tex_t *)&tex, NULL);
    NVNcommandBuffer cmd; CHECK(h_nvnCommandBufferInitialize(&cmd, NULL), "command buffer initialized");
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (uint64_t)mem+131072, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    if (write_after_record) h_nvnTextureWriteTexels(&tex, NULL, &region, pixels);
    // Cached descriptor views must not own every upload after its API object
    // is finalized. Two IDs and a compatible view exercise all lookup caches.
    mtl_reg_tex(2, (const tex_t *)&tex, NULL);
    mtl_sub_begin();
    id<MTLTexture> before = tex_by_id(1), second = tex_by_id(2); uint64_t compatible_id = 0;
    CHECK(before && second && g_treg[1].view && g_treg[2].view, "live descriptors cache their texture");
    CHECK(tex_fit(2, second, MTLTextureType2DArray, &compatible_id) && g_treg[2].alt, "live descriptor caches a compatible view");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "initial descriptor uploads completed");
    h_nvnTextureFinalize(&tex);
    CHECK(mtl_cpu_tex_count() == 0, "finalized texture leaves the live CPU set");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "finalization retires every descriptor's cached views");
    // Reuse the API object: commands and registered descriptors must use their snapshots.
    memset(&tex, 0xa5, sizeof tex);
    state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded texture readback completed");
    CHECK(!mtl_last_commit_error(), "recorded texture readback has no GPU error");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "finalized command snapshot uploads CPU-written pixels");
    mtl_sub_begin(); id<MTLTexture> registered = tex_by_id(1);
    CHECK(registered && registered.width == 4, "finalized registered descriptor resolves a Metal texture");
    CHECK(tex_fit(2, tex_by_id(2), MTLTextureType2DArray, &compatible_id), "late compatible view resolves");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "registered descriptor upload completed");
    CHECK(!g_treg[1].view && !g_treg[2].view && !g_treg[2].alt, "late descriptor views retire at submission end");
    // Resolve again: a retired serial may already hold an NvPkg in the ghost cache.
    mtl_sub_begin(); mtl_exec((const uint8_t *)handle);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull), "recorded snapshot can be replayed again");
    CHECK(!memcmp(mem+131072, pixels, sizeof pixels), "replayed retired snapshot keeps its CPU content");
    mtl_reg_tex(1,NULL,NULL); mtl_reg_tex(2,NULL,NULL); h_nvnCommandBufferFinalize(&cmd);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&pool);
    CHECK(!tex_cpu_written(serial), "pool finalization drops retired CPU classification");
    defer_drain(); munmap(mem,len);
}
int main(int argc, const char **argv) { @autoreleasepool {
    enum { N = 6442, BASE = 1000 };
    static tex_t t[N];
    for (int i = 0; i < N; i++) { t[i].serial = BASE + i; t[i].d.width = t[i].d.height = 4; t[i].d.depth = 1; t[i].d.format = 0x25; mtl_tex_init(&t[i]); }
    CHECK(mtl_cpu_tex_count() == 0 && !tex_cpu_written(BASE), "empty at start");
    for (int i = 0; i < N; i++) mtl_tex_cpu_written(&t[i]);
    mtl_tex_cpu_written(&t[7]);   // again: no duplicate
    CHECK(mtl_cpu_tex_count() == N, "every CPU-written texture counted once");
    int in = 0; for (int i = 0; i < N; i++) in += tex_cpu_written(BASE + i);
    CHECK(in == N, "every one found");
    CHECK(!tex_cpu_written(BASE - 1) && !tex_cpu_written(BASE + N) && !tex_cpu_written(0), "serials outside the set are not in it");
    // Misses: a million lookups of serials past the set, as every resolve of a GPU-only texture asks. With the old set's probing this took seconds; with a mixed hash, far less
    uint64_t t0 = bp_now(); int hits = 0;
    for (int i = 0; i < 1000000; i++) hits += tex_cpu_written(BASE + N + 1 + (uint64_t)i);
    double ms = (bp_now() - t0) / 1e6;
    printf("1,000,000 lookups of serials not in a set of %d consecutive ones: %.1f ms\n", N, ms);
    CHECK(hits == 0, "no miss is reported as a hit");
    CHECK(ms < 1000, "a million misses take well under a second");
    mtl_tex_release(&t[100]); mtl_tex_release(&t[101]);
    CHECK(!tex_cpu_written(BASE + 100) && !tex_cpu_written(BASE + 101) && tex_cpu_written(BASE + 102), "a released texture leaves the set, the others stay");
    CHECK(mtl_cpu_tex_count() == N - 2, "the count follows the releases");
    tex_t z = {0}; mtl_tex_cpu_written(&z);
    CHECK(mtl_cpu_tex_count() == N - 2, "a texture with no serial is not recorded");
    for (int i=0;i<N;i++) mtl_tex_release(&t[i]);
    retired_classification(); descriptor_replacement();
    if (argc > 1 && !strcmp(argv[1], "--metal")) { mtl_init(); finalized_upload(0); finalized_upload(1); }
    printf("t_cpu_tex: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
