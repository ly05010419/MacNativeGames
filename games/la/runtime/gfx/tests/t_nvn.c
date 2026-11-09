// NVN null device unit tests: look up addresses from nvnBootstrapLoader the way the game does and go all the way to present; all expected values are hand-calculated, without calling the implementation under test.
// For build and run see dm/gfx/tests/Makefile (make -C dm/gfx/tests).
#include "../nvn/hle_nvn.c"
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
#include "../tex/nvn_deswizzle.h"
#include <time.h>
void *front_metal_layer(void) { return NULL; }   // no window in unit tests: present only counts
module_t *module_of(const void *p) { (void)p; return NULL; }   // no guest modules in unit tests
module_t g_mod[MAX_MOD]; int g_nmod;                            // host symbols that crash.c depends on
void svc_thread_dump(void) {}
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; } // Tests use host-thread stacks.
void crash_install(void);
void nvn_present_config(int hz, int display_mode);
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }

static int g_fail, g_n;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); g_n++;                 \
    printf("%-44s expected %-12lld → got %-12lld %s\n", name, w_, g_, w_ == g_ ? "✓" : "✗"); if (w_ != g_) g_fail++; } while (0)

#define P(name) ((NVNT_##name)proc(#name))
static PFNNVNDEVICEGETPROCADDRESSPROC gpa; static NVNdevice dev;
static void *proc(const char *n) { void *p = (void *)gpa(&dev, n); if (!p) { printf("address lookup failed %s\n", n); exit(1); } return p; }

// Command memory callback: hands out a new 256-byte memory segment each time (forces a multi-segment JUMP chain)
static uint8_t g_cmdmem[64 * 1024]; static size_t g_cmdoff; static int g_cb_calls; static NVNmemoryPool g_cmdpool;
static void mem_cb(NVNcommandBuffer *c, NVNcommandBufferMemoryEvent ev, size_t minsz, void *data) {
    (void)data; g_cb_calls++;
    if (ev != NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY) return;
    size_t n = minsz > 256 ? minsz : 256;
    P(nvnCommandBufferAddCommandMemory)(c, &g_cmdpool, (ptrdiff_t)g_cmdoff, n); g_cmdoff += n;
}

// Suspend unit test: do the submit on another thread
static _Atomic int g_sstarted, g_sdone; static pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_scv = PTHREAD_COND_INITIALIZER;
static NVNqueue *g_sq; static NVNcommandHandle g_sh; static NVNT_nvnQueueSubmitCommands g_sub;
static void *susp_thr(void *a) { (void)a; pthread_mutex_lock(&g_smu); atomic_store(&g_sstarted, 1); pthread_cond_broadcast(&g_scv); pthread_mutex_unlock(&g_smu); g_sub(g_sq, 1, &g_sh); atomic_store(&g_sdone, 1); return NULL; }
static int wait_started(void) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += 10; pthread_mutex_lock(&g_smu); while (!atomic_load(&g_sstarted) && pthread_cond_timedwait(&g_scv, &g_smu, &dl) == 0) {} int ok = atomic_load(&g_sstarted); pthread_mutex_unlock(&g_smu); return ok; }

static size_t tex_size3(int target, int fmt, int w, int h, int depth) {   // a layered or 3D texture's storage size
    NVNtextureBuilder tb;
    P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb); P(nvnTextureBuilderSetTarget)(&tb, target);
    P(nvnTextureBuilderSetSize3D)(&tb, w, h, depth); P(nvnTextureBuilderSetFormat)(&tb, fmt);
    return P(nvnTextureBuilderGetStorageSize)(&tb);
}
static size_t tex_size(int fmt, int w, int h, int flags, size_t *align) {
    NVNtextureBuilder tb;
    P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetFlags)(&tb, flags); P(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_2D);
    P(nvnTextureBuilderSetWidth)(&tb, w); P(nvnTextureBuilderSetHeight)(&tb, h); P(nvnTextureBuilderSetFormat)(&tb, fmt);
    if (align) *align = P(nvnTextureBuilderGetStorageAlignment)(&tb);
    return P(nvnTextureBuilderGetStorageSize)(&tb);
}

int main(void) {
    crash_install();   // the same SIGSEGV / SIGBUS handling as production (the page-fault hook is dispatched from within it)
    // 1. Bootstrap address lookup: bootstrap gets GetProcAddress, then uses it to get all the names
    PFNNVNBOOTSTRAPLOADERPROC boot = (PFNNVNBOOTSTRAPLOADERPROC)h_nvnBootstrapLoader;
    gpa = (PFNNVNDEVICEGETPROCADDRESSPROC)boot("nvnDeviceGetProcAddress");
    EXPECT("bootstrap GetProcAddress is non-null", 1, gpa != NULL);
    int nonnull = 0; for (unsigned i = 0; i < NVN_NFUNC; i++) nonnull += gpa(NULL, nvn_names[i]) != NULL;
    EXPECT("every name in the name table resolves (NVN_NFUNC)", NVN_NFUNC, nonnull);   // used to be hard-coded 523 and kept failing once the name table grew to 536
    EXPECT("unknown name resolves to null", 0, gpa(NULL, "nvnNoSuchFunction") != NULL);

    // 2. Device
    NVNdeviceBuilder db; P(nvnDeviceBuilderSetDefaults)(&db); P(nvnDeviceBuilderSetFlags)(&db, 0);
    EXPECT("DeviceInitialize succeeds", 1, P(nvnDeviceInitialize)(&dev, &db));
    int v; P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT, &v);
    EXPECT("uniform buffer alignment (256 measured on X1)", 256, v);
    P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE, &v); EXPECT("texture descriptor size (32)", 32, v);
    P(nvnDeviceGetInteger)(&dev, NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE, &v); EXPECT("queue command memory minimum (65536)", 65536, v);

    // 3. Memory pool: GPU address = the storage address given by the game
    static uint8_t poolmem[1 << 20] __attribute__((aligned(4096)));
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    P(nvnMemoryPoolBuilderSetDevice)(&pb, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&pb);
    P(nvnMemoryPoolBuilderSetStorage)(&pb, poolmem, sizeof poolmem); P(nvnMemoryPoolBuilderSetFlags)(&pb, 0x24);
    EXPECT("MemoryPoolInitialize succeeds", 1, P(nvnMemoryPoolInitialize)(&pool, &pb));
    EXPECT("pool GPU address = storage address", (long long)(uintptr_t)poolmem, P(nvnMemoryPoolGetBufferAddress)(&pool));

    // 4. Texture storage size (block-linear: GOB = 64 B × 8 rows; block height from the GOB count of "height × 1.5": ≥16→16, ≥8→8 ...)
    //    256×256 RGBA8: 1024 B per row = 16 GOBs; height 256 rows = 32 GOB rows, block height 16 GOBs → 2 blocks; 16×2×(512×16) = 262144
    size_t al; EXPECT("256x256 RGBA8 size", 262144, tex_size(NVN_FORMAT_RGBA8, 256, 256, 0, &al));
    EXPECT("block-linear alignment", 512, al);
    //    100×100 RGBA8: 400 B → 7 GOBs wide; height 100 → 13 GOB rows, ×1.5 = 150 rows = 19 GOBs → block height 16 → 1 block; 7×1×8192 = 57344
    EXPECT("100x100 RGBA8 size", 57344, tex_size(NVN_FORMAT_RGBA8, 100, 100, 0, NULL));
    //    128×128 DXT1: 32×32 blocks, 8 B/block → 256 B = 4 GOBs wide; 32 rows → ×1.5 = 48 rows = 6 GOBs → block height 4 → 4 GOB rows = 1 block; 4×1×2048 = 8192
    EXPECT("128x128 DXT1 size", 8192, tex_size(NVN_FORMAT_RGBA_DXT1, 128, 128, 0, NULL));
    //    LINEAR 100×100 RGBA8: row pitch 400 aligned to 32 = 416; 416×100 = 41600, aligned to 32
    EXPECT("linear 100x100 RGBA8 size", 41600, tex_size(NVN_FORMAT_RGBA8, 100, 100, NVN_TEXTURE_FLAGS_LINEAR, &al));
    EXPECT("linear alignment", 32, al);
    // A cube array's depth counts layer-faces, as in OpenGL: 12 = two cubes, the size of a 12-layer 2D array (it used to be read as 12 cubes, six times the storage)
    EXPECT("64x64 RGBA8 cube array of depth 12 = 2D array of 12 layers", tex_size3(NVN_TEXTURE_TARGET_2D_ARRAY, NVN_FORMAT_RGBA8, 64, 64, 12),
           tex_size3(NVN_TEXTURE_TARGET_CUBEMAP_ARRAY, NVN_FORMAT_RGBA8, 64, 64, 12));

    // A channel change on the same storage must survive the game's descriptor
    // deduplication. Exercise the public builder/init/compare API, including
    // default versus explicitly specified identity channel selection.
    {
        NVNtextureBuilder tb; NVNtexture original = {0}, replacement = {0};
        P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
        P(nvnTextureBuilderSetSize2D)(&tb, 4, 4);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_R8);
        P(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        P(nvnTextureInitialize)(&original, &tb);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 5);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare implicit/explicit identity", 1, P(nvnTextureCompare)(&original, &replacement));
        P(nvnTextureFinalize)(&replacement);
        P(nvnTextureBuilderSetSwizzle)(&tb, 1, 1, 1, 2);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare A8 channel replacement", 0, P(nvnTextureCompare)(&original, &replacement));
        EXPECT("TextureCompare channel replacement symmetric", 0, P(nvnTextureCompare)(&replacement, &original));
        P(nvnTextureFinalize)(&original); P(nvnTextureFinalize)(&replacement);
        P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGB10A2);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 0);
        P(nvnTextureInitialize)(&original, &tb);
        P(nvnTextureBuilderSetSwizzle)(&tb, 2, 3, 4, 1);
        P(nvnTextureInitialize)(&replacement, &tb);
        EXPECT("TextureCompare capture alpha zero/one", 0, P(nvnTextureCompare)(&original, &replacement));
        EXPECT("TextureCompare same capture descriptor", 1, P(nvnTextureCompare)(&replacement, &replacement));
        P(nvnTextureFinalize)(&original); P(nvnTextureFinalize)(&replacement);
    }

    // Flushed ranges (nvn_flushed): a texture's storage flushed before its first use is filled from memory (tex_init_fill). Spans merge when they overlap or touch, and go with their pool
    {
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1000, 0x100);
        EXPECT("flushed: the flushed range", 1, nvn_flushed(poolmem + 0x1000, 0x100));
        EXPECT("flushed: its last byte", 1, nvn_flushed(poolmem + 0x10ff, 1));
        EXPECT("flushed: a range ending at its start", 0, nvn_flushed(poolmem + 0xf00, 0x100));
        EXPECT("flushed: a range starting at its end", 0, nvn_flushed(poolmem + 0x1100, 0x100));
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1100, 0x100);
        EXPECT("flushed: a touching flush merges (1 span)", 1, (long long)g_nfl);
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x3000, 0x10);
        EXPECT("flushed: a separate flush (2 spans)", 2, (long long)g_nfl);
        EXPECT("flushed: the gap between them", 0, nvn_flushed(poolmem + 0x1200, 0x1e00));
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0x1080, 0x40);
        EXPECT("flushed: a flush inside a span (still 2)", 2, (long long)g_nfl);
        P(nvnMemoryPoolFlushMappedRange)(&pool, 0, 0x4000);
        EXPECT("flushed: a flush over both (1 span)", 1, (long long)g_nfl);
        fl_drop(poolmem + 0x1000, 0x1000);   // a pool inside the span: split
        EXPECT("flushed: a pool dropped from inside a span (2 spans)", 2, (long long)g_nfl);
        EXPECT("flushed: the dropped part", 0, nvn_flushed(poolmem + 0x1000, 0x1000));
        EXPECT("flushed: before it", 1, nvn_flushed(poolmem + 0xfff, 1));
        EXPECT("flushed: after it", 1, nvn_flushed(poolmem + 0x2000, 1));
        static uint8_t smem[0x2000] __attribute__((aligned(4096))); NVNmemoryPool sp;
        P(nvnMemoryPoolBuilderSetStorage)(&pb, smem, sizeof smem);
        P(nvnMemoryPoolInitialize)(&sp, &pb);
        P(nvnMemoryPoolFlushMappedRange)(&sp, 0x800, 0x800);
        EXPECT("flushed: a second pool's flush", 1, nvn_flushed(smem + 0x800, 1));
        P(nvnMemoryPoolFinalize)(&sp);
        EXPECT("flushed: gone with its pool", 0, nvn_flushed(smem, sizeof smem));
        fl_drop(poolmem, sizeof poolmem);
        EXPECT("flushed: the first pool's spans dropped", 0, (long long)g_nfl);
        P(nvnMemoryPoolBuilderSetStorage)(&pb, poolmem, sizeof poolmem);
    }

    // 5. Command buffer: record uniform updates / events / counters / buffer copies / nested calls, with memory in 256 B segments to force multi-segment linking
    NVNcommandBuffer cb; P(nvnCommandBufferInitialize)(&cb, &dev);
    NVNmemoryPoolBuilder cpb; P(nvnMemoryPoolBuilderSetDefaults)(&cpb); P(nvnMemoryPoolBuilderSetStorage)(&cpb, g_cmdmem, sizeof g_cmdmem);
    P(nvnMemoryPoolInitialize)(&g_cmdpool, &cpb);
    P(nvnCommandBufferSetMemoryCallback)(&cb, mem_cb); P(nvnCommandBufferSetMemoryCallbackData)(&cb, NULL);
    NVNeventBuilder eb; NVNevent ev; P(nvnEventBuilderSetDefaults)(&eb); P(nvnEventBuilderSetStorage)(&eb, &pool, 0x1000);
    P(nvnEventInitialize)(&ev, &eb);
    uint8_t *ub = poolmem + 0x2000, *cnt = poolmem + 0x3000, *src = poolmem + 0x4000, *dst = poolmem + 0x5000;
    for (int i = 0; i < 64; i++) src[i] = (uint8_t)(i * 3 + 1);
    uint32_t data[16];

    P(nvnCommandBufferBeginRecording)(&cb);
    EXPECT("recording", 1, P(nvnCommandBufferIsRecording)(&cb));
    for (int k = 0; k < 20; k++) {                        // 20 updates to different offsets of the same buffer, 64 B of data each
        for (int i = 0; i < 16; i++) data[i] = k * 100 + i;
        P(nvnCommandBufferUpdateUniformBuffer)(&cb, (NVNbufferAddress)ub, 4096, k * 64, 64, data);
        memset(data, 0xEE, sizeof data);                  // modify the original data after recording: the submit must use the values copied in at record time
    }
    P(nvnCommandBufferSignalEvent)(&cb, &ev, NVN_EVENT_SIGNAL_MODE_WRITE, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 7);
    P(nvnCommandBufferSignalEvent)(&cb, &ev, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 5);
    P(nvnCommandBufferReportCounter)(&cb, NVN_COUNTER_TYPE_TIMESTAMP, (NVNbufferAddress)cnt);
    NVNcommandHandle h1 = P(nvnCommandBufferEndRecording)(&cb);
    EXPECT("not recording after EndRecording", 0, P(nvnCommandBufferIsRecording)(&cb));
    P(nvnCommandBufferBeginRecording)(&cb);
    P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)src, (NVNbufferAddress)dst, 64, 0);
    P(nvnCommandBufferCopyCommands)(&cb, 1, &h1);         // nested: execute h1 once more (event 7 → +5 → write 7 again → +5)
    NVNcommandHandle h2 = P(nvnCommandBufferEndRecording)(&cb);
    printf("(command memory callback called %d times, %zu B used)\n", g_cb_calls, g_cmdoff);
    EXPECT("command memory callback at least twice (multi-segment)", 1, g_cb_calls >= 2);
    EXPECT("event value still 0 before submit", 0, P(nvnEventGetValue)(&ev));

    NVNqueueBuilder qb; NVNqueue q; static uint8_t qmem[1 << 19];
    P(nvnQueueBuilderSetDevice)(&qb, &dev); P(nvnQueueBuilderSetDefaults)(&qb);
    size_t qsz = P(nvnQueueBuilderGetQueueMemorySize)(&qb);
    EXPECT("queue memory = 64K+256K+16K rounded to 4K", 65536 + 262144 + 16384, qsz);
    P(nvnQueueBuilderSetQueueMemory)(&qb, qmem, qsz);
    EXPECT("QueueInitialize succeeds", 1, P(nvnQueueInitialize)(&q, &qb));
    NVNcommandHandle hs[2] = {h1, h2};
    P(nvnQueueSubmitCommands)(&q, 2, hs);
    NVNsync sf; P(nvnSyncInitialize)(&sf, &dev); P(nvnQueueFenceSync)(&q, &sf, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    int wr = P(nvnSyncWait)(&sf, 5000000000ull);
    EXPECT("fence wait after submit succeeds (0 or 1)", 1, wr == 0 || wr == 1);
    int ub_ok = 1;
    for (int k = 0; k < 20; k++) for (int i = 0; i < 16; i++) { uint32_t w; memcpy(&w, ub + k * 64 + i * 4, 4); ub_ok &= w == (uint32_t)(k * 100 + i); }
    EXPECT("uniform buffer 20×16 words equal the recorded values", 1, ub_ok);
    EXPECT("event value: h1(7,+5), then h2 copies and runs h1(7,+5) again = 12", 12, P(nvnEventGetValue)(&ev));
    EXPECT("buffer copy 64 B identical", 0, memcmp(src, dst, 64));
    uint64_t ts[2]; memcpy(ts, cnt, 16);
    EXPECT("counter timestamp written (non-zero)", 1, ts[0] != 0 && ts[0] == ts[1]);

    // 5a. Command memory per recording, as GetCommandMemoryUsed reports it (games size prerecorded command blocks by it: Breath of the Wild records one scratch draw per mesh):
    //     no BEGIN record, a plain indexed draw without the indirect / multi-draw fields, and the handle is the first record; an indirect draw keeps its address
    {
        static uint8_t m[4096] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c2;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c2, &dev);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferDrawElementsBaseVertex)(&c2, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 36, 0x10000, 5);
        NVNcommandHandle hd = P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("plain indexed draw recording: 0x28 + END", 0x30, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        op_hdr rh; memcpy(&rh, m, sizeof rh); p_draw_idx dt; const p_draw_idx *d = draw_idx_rec(m + 8, rh.size, &dt);
        EXPECT("its handle is the draw record", 1, (const uint8_t *)hd == m && rh.op == OP_DRAW_IDX);
        EXPECT("draw read back: count, base vertex, no indirect address", 1, d->count == 36 && d->base_vertex == 5 && d->ibuf == 0x10000 && !d->ind && !d->mdi_cnt);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2); NVNcommandHandle he = P(nvnCommandBufferEndRecording)(&c2);
        memcpy(&rh, m, sizeof rh);
        EXPECT("empty recording: its END only", 8, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        EXPECT("empty recording's handle is its END", 1, (const uint8_t *)he == m && rh.op == OP_END);
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        P(nvnCommandBufferDrawElementsIndirect)(&c2, NVN_DRAW_PRIMITIVE_TRIANGLES, NVN_INDEX_TYPE_UNSIGNED_SHORT, 0x10000, 0x20000);
        P(nvnCommandBufferEndRecording)(&c2);
        memcpy(&rh, m, sizeof rh); d = draw_idx_rec(m + 8, rh.size, &dt);
        EXPECT("indirect draw keeps its address", 0x20000, d->ind);
        // A single texture bind keeps its handle 16 bytes into the command, where a game that patches recorded commands writes a new one (Luigi's Mansion 3: base + GetCommandMemoryUsed + 0x10)
        P(nvnCommandBufferAddCommandMemory)(&c2, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c2);
        size_t at = P(nvnCommandBufferGetCommandMemoryUsed)(&c2);
        P(nvnCommandBufferBindTexture)(&c2, NVN_SHADER_STAGE_FRAGMENT, 3, (NVNtextureHandle)(0x1ff00000u | 1234));
        P(nvnCommandBufferEndRecording)(&c2);
        EXPECT("single texture bind recording: 0x20 + END", 0x28, P(nvnCommandBufferGetCommandMemoryUsed)(&c2));
        uint32_t patched = 0x1ff00000u | 1235; memcpy(m + at + 0x10, &patched, 4);
        memcpy(&rh, m, sizeof rh); p_bind_handles bt; const p_bind_handles *bh = bind_rec(rh.op, m + 8, &bt);
        EXPECT("the bind reads the handle patched at its start + 0x10: slot 3, texture 1235, sampler 511", 1, rh.op == OP_BIND_TEX1 && bh->n == 1 && bh->first == 3 && bh->stage == NVN_SHADER_STAGE_FRAGMENT && bh->h[0] == patched);
        P(nvnCommandBufferFinalize)(&c2); P(nvnMemoryPoolFinalize)(&mp);
    }
    // 5b'. Fixed slots sized by GetCommandMemoryUsed, as Tears of the Kingdom gives each model's prerecorded lists (GitHub issue #61): a list measured at S bytes is recorded
    //      into slot k = base + S × k with AddCommandMemory(max(S, 512)), and slot k + 1 is recorded right after it. The list here is an uniform update of n bytes and a
    //      uniform buffer bind (the last record before the END in the crash), the next slot's starts with a CALL. Walking slot 0 afterwards must reach its own END through
    //      its two records; with 24 bytes kept for a JUMP or an END, S = 496 put its END in a host segment and the JUMP at 488 had its target overwritten by that CALL
    {
        static uint8_t m[8192] __attribute__((aligned(4096))); NVNmemoryPoolBuilder mb; NVNmemoryPool mp; NVNcommandBuffer c3;
        P(nvnMemoryPoolBuilderSetDefaults)(&mb); P(nvnMemoryPoolBuilderSetStorage)(&mb, m, sizeof m); P(nvnMemoryPoolInitialize)(&mp, &mb);
        P(nvnCommandBufferInitialize)(&c3, &dev);
        static uint8_t ub[1024]; NVNcommandHandle other = (NVNcommandHandle)m;   // the CALL's target is never followed here
        int broken = 0, jumps_fit = 0, first_bad = -1;
        for (int n = 0; n <= 1000; n += 8) {
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, sizeof m); P(nvnCommandBufferBeginRecording)(&c3);   // measure, in ample memory
            P(nvnCommandBufferUpdateUniformBuffer)(&c3, 0x10000, 0x1000, 0, (size_t)n, ub); P(nvnCommandBufferBindUniformBuffer)(&c3, NVN_SHADER_STAGE_FRAGMENT, 4, 0x20000, 0x100);
            P(nvnCommandBufferEndRecording)(&c3);
            size_t S = P(nvnCommandBufferGetCommandMemoryUsed)(&c3), give = S > 512 ? S : 512;
            memset(m, 0, sizeof m);
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, 0, give); P(nvnCommandBufferBeginRecording)(&c3);
            P(nvnCommandBufferUpdateUniformBuffer)(&c3, 0x10000, 0x1000, 0, (size_t)n, ub); P(nvnCommandBufferBindUniformBuffer)(&c3, NVN_SHADER_STAGE_FRAGMENT, 4, 0x20000, 0x100);
            const uint8_t *h0 = (const uint8_t *)P(nvnCommandBufferEndRecording)(&c3);
            P(nvnCommandBufferAddCommandMemory)(&c3, &mp, (ptrdiff_t)S, give); P(nvnCommandBufferBeginRecording)(&c3);   // the next slot, right behind
            P(nvnCommandBufferCallCommands)(&c3, 1, &other); P(nvnCommandBufferEndRecording)(&c3);
            // walk slot 0 as mtl_exec does: JUMP targets must be in the pool or a host segment
            const uint8_t *p = h0; int ops[4], k = 0, jumped = 0, ok = 1;
            for (int guard = 0; guard < 8; guard++) {
                op_hdr h; memcpy(&h, p, sizeof h);
                if (h.op == OP_JUMP) { const uint8_t *t; memcpy(&t, p + 8, 8); int in = t >= m && t < m + sizeof m;
                    for (int i = 0; i < g_nhostseg; i++) in |= t >= g_hostseg[i].mem && t < g_hostseg[i].mem + HOSTSEG_SZ;
                    if (!in) { ok = 0; break; } jumped = 1; p = t; continue; }
                if (k < 4) ops[k] = (int)h.op; k++;
                if (h.op == OP_END) break;
                p += h.size;
            }
            ok &= k == 3 && ops[0] == OP_UPDATE_UB && ops[1] == OP_BIND_UBO && ops[2] == OP_END;
            if (!ok) { broken++; if (first_bad < 0) first_bad = (int)S; }
            jumps_fit += !jumped && S <= 504;   // fits in its 512 bytes with a JUMP's room to spare after its last record: stays in its slot
        }
        printf("(first broken slot size %d)\n", first_bad);
        EXPECT("slotted lists replay whole after the next slot is recorded", 0, broken);
        EXPECT("lists of 64..504 bytes stay in their 512 (no host segment)", (504 - 64) / 8 + 1, jumps_fit);
        P(nvnCommandBufferFinalize)(&c3); P(nvnMemoryPoolFinalize)(&mp);
    }

    // 5b. Sync must hold the CPU back: first queue 24 copies of 64 MiB to keep the GPU busy, with the last one copying src to dst;
    //     the CPU rewrites src only after the fence wait returns. If the wait is released early, that last copy reads the rewritten src.
    {
        size_t big = 64u << 20, pgsz = 16384;
        uint8_t *heap; posix_memalign((void **)&heap, pgsz, 4 * big);
        uint8_t *busy_a = heap, *busy_b = heap + big, *bsrc = heap + 2 * big, *bdst = heap + 3 * big;
        memset(heap, 0, 4 * big); memset(bsrc, 0x5A, big);
        NVNmemoryPoolBuilder hb; NVNmemoryPool hp; P(nvnMemoryPoolBuilderSetDefaults)(&hb); P(nvnMemoryPoolBuilderSetStorage)(&hb, heap, 4 * big);
        P(nvnMemoryPoolInitialize)(&hp, &hb);
        P(nvnCommandBufferBeginRecording)(&cb);
        for (int i = 0; i < 24; i++) P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)busy_a, (NVNbufferAddress)busy_b, big, 0);
        P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)bsrc, (NVNbufferAddress)bdst, big, 0);
        NVNcommandHandle hb2 = P(nvnCommandBufferEndRecording)(&cb);
        NVNsync sb; P(nvnSyncInitialize)(&sb, &dev);
        uint64_t t0 = now_ns();
        P(nvnQueueSubmitCommands)(&q, 1, &hb2); P(nvnQueueFenceSync)(&q, &sb, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
        int w0 = P(nvnSyncWait)(&sb, 0);
        int w1 = P(nvnSyncWait)(&sb, 10000000000ull);
        double ms = (now_ns() - t0) / 1e6;
        memset(bsrc, 0xC3, big);                                       // rewrite only after the wait is released
        size_t bad = 0; for (size_t i = 0; i < big; i += 4096) bad += bdst[i] != 0x5A;
        printf("(GPU busy for about %.1f ms)\n", ms);
        EXPECT("zero-timeout wait right after submit = timeout (2)", 2, w0);
        EXPECT("long wait = condition satisfied (1)", 1, w1);
        EXPECT("dst holds only the pre-rewrite value (16384 samples)", 0, bad);
        P(nvnMemoryPoolFinalize)(&hp); free(heap);
    }

    // 6. Sync / window / present
    NVNsync s1, s2; P(nvnSyncInitialize)(&s1, &dev); P(nvnSyncInitialize)(&s2, &dev);
    EXPECT("wait on an unfenced sync = timeout (2)", 2, P(nvnSyncWait)(&s1, 0));
    P(nvnQueueFenceSync)(&q, &s1, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    EXPECT("wait after fence = already signaled (0)", 0, P(nvnSyncWait)(&s1, 0));
    NVNtexture sc[2]; NVNtextureBuilder tb; P(nvnTextureBuilderSetDevice)(&tb, &dev); P(nvnTextureBuilderSetDefaults)(&tb);
    P(nvnTextureBuilderSetWidth)(&tb, 1280); P(nvnTextureBuilderSetHeight)(&tb, 720); P(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
    P(nvnTextureBuilderSetStorage)(&tb, &pool, 0); P(nvnTextureInitialize)(&sc[0], &tb); P(nvnTextureInitialize)(&sc[1], &tb);
    EXPECT("texture width", 1280, P(nvnTextureGetWidth)(&sc[0]));
    NVNtexture *scp[2] = {&sc[0], &sc[1]};
    NVNwindowBuilder wb; NVNwindow win; P(nvnWindowBuilderSetDevice)(&wb, &dev); P(nvnWindowBuilderSetDefaults)(&wb);
    P(nvnWindowBuilderSetTextures)(&wb, 2, scp); EXPECT("WindowInitialize succeeds", 1, P(nvnWindowInitialize)(&win, &wb));
    int idx[4];
    for (int f = 0; f < 4; f++) {
        P(nvnWindowAcquireTexture)(&win, &s2, &idx[f]);
        for (int b = 0; b < 5; b++) P(nvnCommandBufferBarrier)(&cb, 0);
        P(nvnQueuePresentTexture)(&q, &win, idx[f]);
    }
    EXPECT("swapchain rotates 0,1,0,1", 0x0101, idx[0] << 12 | idx[1] << 8 | idx[2] << 4 | idx[3]);
    EXPECT("sync signaled after acquire (0)", 0, P(nvnSyncWait)(&s2, 0));
    unsigned bi = 0; while (strcmp(nvn_names[bi], "nvnCommandBufferBarrier")) bi++;
    EXPECT("Barrier count = 4 frames × 5", 20, nvn_slot[bi].cnt);

    // 6b. Readback: pixel (x,y) = {x*16+y, y*16+x, 0x55, 0xAA} (expected values computed directly from the formula)
    {
        #define PX(x, y, c) ((uint8_t)((c) == 0 ? (x) * 16 + (y) : (c) == 1 ? (y) * 16 + (x) : (c) == 2 ? 0x55 : 0xAA))
        static uint8_t rb[256 << 10] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder rpb; NVNmemoryPool rp; P(nvnMemoryPoolBuilderSetDefaults)(&rpb); P(nvnMemoryPoolBuilderSetStorage)(&rpb, rb, sizeof rb);
        P(nvnMemoryPoolInitialize)(&rp, &rpb);
        uint8_t *stage = rb + 0x8000, *outb = rb + 0xC000;
        for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) for (int c = 0; c < 4; c++) stage[(y * 8 + x) * 4 + c] = PX(x, y, c);
        NVNtextureBuilder b; NVNtexture ta; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
        P(nvnTextureBuilderSetWidth)(&b, 8); P(nvnTextureBuilderSetHeight)(&b, 4); P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8);
        P(nvnTextureBuilderSetStorage)(&b, &rp, 0); P(nvnTextureInitialize)(&ta, &b);
        NVNcopyRegion all = {0, 0, 0, 8, 4, 1}, sub = {2, 1, 0, 5, 3, 1}, cp = {1, 0, 0, 4, 2, 1};
        P(nvnCommandBufferBeginRecording)(&cb);
        P(nvnCommandBufferSetCopyRowStride)(&cb, 0);
        P(nvnCommandBufferCopyBufferToTexture)(&cb, (NVNbufferAddress)stage, &ta, NULL, &all, 0);
        P(nvnCommandBufferSetCopyRowStride)(&cb, 64);
        P(nvnCommandBufferCopyTextureToBuffer)(&cb, &ta, NULL, &cp, (NVNbufferAddress)outb, 0);
        NVNcommandHandle hr = P(nvnCommandBufferEndRecording)(&cb);
        memset(outb, 0xEE, 256);
        P(nvnQueueSubmitCommands)(&q, 1, &hr); P(nvnQueueFinish)(&q);
        uint8_t got[5 * 3 * 4]; P(nvnTextureReadTexels)(&ta, NULL, &sub, got);
        int bad = 0; for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++) for (int c = 0; c < 4; c++) bad += got[(y * 5 + x) * 4 + c] != PX(x + 2, y + 1, c);
        EXPECT("ReadTexels 5x3 sub-region byte for byte (GPU-written texture)", 0, bad);
        uint8_t gs[4 * 40]; memset(gs, 0xEE, sizeof gs); P(nvnTextureReadTexelsStrided)(&ta, NULL, &all, gs, 40, 0);
        bad = 0; for (int y = 0; y < 4; y++) { for (int x = 0; x < 8; x++) for (int c = 0; c < 4; c++) bad += gs[y * 40 + x * 4 + c] != PX(x, y, c); for (int k = 32; k < 40; k++) bad += gs[y * 40 + k] != 0xEE; }
        EXPECT("ReadTexelsStrided stride 40 (row padding untouched)", 0, bad);
        bad = 0; for (int y = 0; y < 2; y++) { for (int x = 0; x < 4; x++) for (int c = 0; c < 4; c++) bad += outb[y * 64 + x * 4 + c] != PX(x + 1, y, c); for (int k = 16; k < 64; k++) bad += outb[y * 64 + k] != 0xEE; }
        EXPECT("CopyTextureToBuffer stride 64 (GPU copy-back)", 0, bad);
        // Packaged texture: the pool memory is the ground truth (block-linear); ReadTexels deswizzles and reads it directly
        uint8_t lin[16 * 8 * 4]; for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 4; c++) lin[(y * 16 + x) * 4 + c] = PX(x, y, c);
        NVNtexture tb; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
        P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetWidth)(&b, 16); P(nvnTextureBuilderSetHeight)(&b, 8); P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8);
        P(nvnTextureBuilderSetPackagedTextureLayout)(&b, &lay); P(nvnTextureBuilderSetStorage)(&b, &rp, 0x10000); P(nvnTextureInitialize)(&tb, &b);
        nvn_texdesc dd = ((tex_t *)&tb)->d;
        if (nvn_swizzle(&dd, 0, 0, lin, 64, rb + 0x10000)) { printf("nvn_swizzle failed\n"); g_fail++; }
        NVNcopyRegion s2 = {3, 2, 0, 9, 5, 1}; uint8_t g2[9 * 5 * 4];
        P(nvnTextureReadTexels)(&tb, NULL, &s2, g2);
        bad = 0; for (int y = 0; y < 5; y++) for (int x = 0; x < 9; x++) for (int c = 0; c < 4; c++) bad += g2[(y * 9 + x) * 4 + c] != PX(x + 3, y + 2, c);
        EXPECT("ReadTexels packaged texture 9x5 (block-linear pool memory read directly)", 0, bad);
        P(nvnMemoryPoolFinalize)(&rp);
    }

    // 6c. Write-protected dirty pages: after a packaged texture is uploaded its pool pages are read-only; game write → page fault → mark dirty → re-upload on next use; a direct host read() write needs prepare first
    {
        static uint8_t wb_[256 << 10] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder wpb; NVNmemoryPool wp; P(nvnMemoryPoolBuilderSetDefaults)(&wpb); P(nvnMemoryPoolBuilderSetStorage)(&wpb, wb_, sizeof wb_);
        P(nvnMemoryPoolInitialize)(&wp, &wpb);
        NVNtextureBuilder b; NVNtexture tw; NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}};
        P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b); P(nvnTextureBuilderSetWidth)(&b, 16); P(nvnTextureBuilderSetHeight)(&b, 8);
        P(nvnTextureBuilderSetFormat)(&b, NVN_FORMAT_RGBA8); P(nvnTextureBuilderSetPackagedTextureLayout)(&b, &lay);
        P(nvnTextureBuilderSetStorage)(&b, &wp, 0x10000); P(nvnTextureInitialize)(&tw, &b);
        nvn_texdesc dd = ((tex_t *)&tw)->d;
        uint8_t lin[16 * 8 * 4], *store = wb_ + 0x10000, *outb = wb_ + 0x20000;
        #define FILLPAT(k) for (int y = 0; y < 8; y++) for (int x = 0; x < 16; x++) for (int c = 0; c < 4; c++) lin[(y * 16 + x) * 4 + c] = (uint8_t)(x * 16 + y + (k) * 3 + c * 41)
        FILLPAT(0); nvn_swizzle(&dd, 0, 0, lin, 64, store);
        NVNcopyRegion all = {0, 0, 0, 16, 8, 1};
        // Check the actual GPU-side contents with a GPU copy-back (CopyTextureToBuffer reading the MTLTexture): expected = pattern version k
        #define GPU_READ_EQ(k, name) do { P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferSetCopyRowStride)(&cb, 0); \
            P(nvnCommandBufferCopyTextureToBuffer)(&cb, &tw, NULL, &all, (NVNbufferAddress)outb, 0); NVNcommandHandle hh = P(nvnCommandBufferEndRecording)(&cb); \
            P(nvnQueueSubmitCommands)(&q, 1, &hh); P(nvnQueueFinish)(&q); FILLPAT(k); EXPECT(name, 0, memcmp(outb, lin, sizeof lin)); } while (0)
        uint64_t up0, re0, fa0, pr0, up, re, fa, pr; mtl_pkg_stats(&up0, &re0); nvn_dirty_stats(&fa0, &pr0);
        GPU_READ_EQ(0, "first upload: GPU-side contents = version 0");
        mtl_pkg_stats(&up, &re); EXPECT("first uploads +1", 1, up - up0);
        FILLPAT(1); nvn_swizzle(&dd, 0, 0, lin, 64, store);          // game-style CPU write: protected page → page fault → hook marks it dirty
        nvn_dirty_stats(&fa, &pr); EXPECT("game write triggers ≥ 1 page fault (caught by the hook, no crash)", 1, fa - fa0 >= 1);
        GPU_READ_EQ(1, "after re-upload: GPU-side contents = version 1");
        mtl_pkg_stats(&up, &re); EXPECT("pool contents changed: re-uploads +1", 1, re - re0);
        uint64_t fa1 = fa; GPU_READ_EQ(1, "reuse with unchanged contents: still version 1");
        nvn_dirty_stats(&fa, &pr); mtl_pkg_stats(&up, &re); EXPECT("unchanged contents: page faults +0", 0, fa - fa1); EXPECT("unchanged contents: re-uploads still 1", 1, re - re0);
        // Pitfall 1: host read() writing directly into protected pages. Without prepare the kernel returns EFAULT (no page fault); after prepare the read succeeds, the pages are marked dirty and re-uploaded next time
        char path[1024]; snprintf(path, sizeof path, "%s/t_nvn_rd_XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "."); int fd = mkstemp(path);   // per-user temp directory, do not write to /tmp
        FILLPAT(2); uint8_t tmpst[sizeof lin * 4]; memset(tmpst, 0, sizeof tmpst);
        static uint8_t full[0x10000]; memcpy(full, store, sizeof full);   // existing storage contents (block-linear); write version 2 into a copy and then save it to a file
        nvn_swizzle(&dd, 0, 0, lin, 64, full); if (write(fd, full, ((tex_t *)&tw)->L.size) < 0) g_fail++;
        ssize_t r0 = pread(fd, store, ((tex_t *)&tw)->L.size, 0); int e0 = errno;
        EXPECT("without prepare: read() into protected pages returns -1 with EFAULT", 1, r0 == -1 && e0 == EFAULT);
        dm_guest_write_prepare(store, ((tex_t *)&tw)->L.size);
        ssize_t r1 = pread(fd, store, ((tex_t *)&tw)->L.size, 0);
        EXPECT("read() succeeds after prepare", (long long)((tex_t *)&tw)->L.size, r1);
        close(fd); unlink(path);
        GPU_READ_EQ(2, "re-upload after host write: GPU-side contents = version 2");
        mtl_pkg_stats(&up, &re); EXPECT("host write triggers a re-upload, 2 in total", 2, re - re0);
        P(nvnMemoryPoolFinalize)(&wp);
    }

    // 6c2. WriteTexels region writes: the result must be byte-identical to "decode the whole level → patch the region → re-encode" (Metroid Dread's minimap writes one texel at a time, which used to cost a whole-level round trip each).
    //      RGBA8 100×37 (width not a whole number of GOBs), BC1 64×64 (4×4 blocks, 8 bytes per block), RGBA16F 33×17 two-layer array; several regions each, including 1×1, a whole row, GOB-crossing and the whole level
    {
        static uint8_t wtmem[1 << 18] __attribute__((aligned(16384)));
        NVNmemoryPoolBuilder wb2; NVNmemoryPool wtp; P(nvnMemoryPoolBuilderSetDevice)(&wb2, &dev); P(nvnMemoryPoolBuilderSetDefaults)(&wb2);
        P(nvnMemoryPoolBuilderSetStorage)(&wb2, wtmem, sizeof wtmem); P(nvnMemoryPoolBuilderSetFlags)(&wb2, 0x24); P(nvnMemoryPoolInitialize)(&wtp, &wb2);
        struct { int fmt, w, h, layers; int rg[5][4]; } cs[] = {
            {NVN_FORMAT_RGBA8, 100, 37, 1, {{0, 0, 1, 1}, {99, 36, 1, 1}, {13, 5, 40, 9}, {0, 20, 100, 1}, {0, 0, 100, 37}}},
            {NVN_FORMAT_RGBA_DXT1, 64, 64, 1, {{0, 0, 4, 4}, {60, 60, 4, 4}, {16, 8, 32, 20}, {0, 32, 64, 4}, {0, 0, 64, 64}}},
            {NVN_FORMAT_RGBA16F, 33, 17, 2, {{32, 16, 1, 1}, {1, 2, 30, 13}, {0, 0, 33, 1}, {5, 9, 17, 8}, {0, 0, 33, 17}}},
        };
        int bad = 0, n = 0; uint32_t seed = 12345;
        for (unsigned c = 0; c < sizeof cs / sizeof cs[0]; c++) {
            NVNtextureBuilder b; NVNtexture t; P(nvnTextureBuilderSetDevice)(&b, &dev); P(nvnTextureBuilderSetDefaults)(&b);
            P(nvnTextureBuilderSetTarget)(&b, cs[c].layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D);
            P(nvnTextureBuilderSetSize3D)(&b, cs[c].w, cs[c].h, cs[c].layers); P(nvnTextureBuilderSetFormat)(&b, cs[c].fmt);
            P(nvnTextureBuilderSetStorage)(&b, &wtp, 0); P(nvnTextureInitialize)(&t, &b);
            const tex_t *tt = (const tex_t *)&t; int bw, bh, bpb = nvn_fmt_bpb(cs[c].fmt, &bw, &bh);
            size_t lp = (size_t)((cs[c].w + bw - 1) / bw) * bpb, lr = (size_t)(cs[c].h + bh - 1) / bh;
            for (int k = 0; k < 5; k++) {
                for (size_t i = 0; i < tt->L.size; i++) wtmem[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) >> 3;   // random old content
                static uint8_t want[1 << 18], lin[1 << 16], src[1 << 16];
                int x = cs[c].rg[k][0], y = cs[c].rg[k][1], w = cs[c].rg[k][2], h = cs[c].rg[k][3], z = cs[c].layers - 1;
                size_t wbb = (size_t)((w + bw - 1) / bw) * bpb, hbb = (size_t)(h + bh - 1) / bh;
                for (size_t i = 0; i < wbb * hbb; i++) src[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) >> 5;
                memcpy(want, wtmem, tt->L.size);   // expected: decode the whole level, patch, re-encode (only the last layer)
                nvn_deswizzle(&tt->d, 0, (unsigned)z, want, lin, lp);
                for (size_t r = 0; r < hbb; r++) memcpy(lin + ((size_t)y / bh + r) * lp + (size_t)(x / bw) * bpb, src + r * wbb, wbb);
                nvn_swizzle(&tt->d, 0, (unsigned)z, lin, lp, want); (void)lr;
                NVNtextureView vw; P(nvnTextureViewSetDefaults)(&vw); P(nvnTextureViewSetLayers)(&vw, z, 1);
                NVNcopyRegion rg = {x, y, 0, w, h, 1};
                write_texels(tt, cs[c].layers > 1 ? (const view_t *)&vw : NULL, &rg, src, 0, 0);
                n++; if (memcmp(want, wtmem, tt->L.size)) bad++;
            }
            P(nvnTextureFinalize)(&t);
        }
        EXPECT("WriteTexels region write byte-identical to a whole-level round trip (15 cases)", 0, bad); EXPECT("WriteTexels region case count", 15, n);
        P(nvnMemoryPoolFinalize)(&wtp);
    }

    // 6d. Lifecycle: in-flight GPU work is 0 when suspend returns; submits are blocked during suspend and let through after resume
    {
        size_t big = 64u << 20; uint8_t *heap; posix_memalign((void **)&heap, 16384, 2 * big); memset(heap, 0, 2 * big);
        NVNmemoryPoolBuilder hb; NVNmemoryPool hp; P(nvnMemoryPoolBuilderSetDefaults)(&hb); P(nvnMemoryPoolBuilderSetStorage)(&hb, heap, 2 * big);
        P(nvnMemoryPoolInitialize)(&hp, &hb);
        P(nvnCommandBufferBeginRecording)(&cb);
        for (int i = 0; i < 24; i++) P(nvnCommandBufferCopyBufferToBuffer)(&cb, (NVNbufferAddress)heap, (NVNbufferAddress)(heap + big), big, 0);
        NVNcommandHandle hbusy = P(nvnCommandBufferEndRecording)(&cb);
        P(nvnQueueSubmitCommands)(&q, 1, &hbusy);
        uint64_t f = mtl_last_fence();
        int busy_before = !mtl_wait(f, 0);
        dm_gfx_suspend(1);
        EXPECT("GPU really busy before suspend (zero-timeout wait not complete)", 1, busy_before);
        EXPECT("in-flight work complete when suspend returns", 1, mtl_wait(f, 0));
        P(nvnCommandBufferBeginRecording)(&cb); P(nvnCommandBufferBarrier)(&cb, 0); NVNcommandHandle hsmall = P(nvnCommandBufferEndRecording)(&cb);
        g_sq = &q; g_sh = hsmall; g_sub = P(nvnQueueSubmitCommands); atomic_store(&g_sstarted, 0); atomic_store(&g_sdone, 0);
        pthread_t th; pthread_create(&th, NULL, susp_thr, NULL);
        int started = wait_started();
        EXPECT("during suspend another thread entered submit and has not returned", 1, started && !atomic_load(&g_sdone));
        dm_gfx_suspend(0);
        pthread_join(th, NULL);
        EXPECT("that submit is let through and returns after resume", 1, atomic_load(&g_sdone));
        P(nvnQueueFinish)(&q); P(nvnMemoryPoolFinalize)(&hp); free(heap);
    }

    // 6e. Present pacing (real-device semantics): present returns immediately; blocking happens at acquire - swapchain of 3, interval 1, 60 Hz, the 4th acquire waits until texture 0 has been displayed for a full period
    {
        nvn_present_config(60, 0);
        NVNwindowBuilder wb2; NVNwindow w2; P(nvnWindowBuilderSetDevice)(&wb2, &dev); P(nvnWindowBuilderSetDefaults)(&wb2);
        NVNtexture *sc3[3] = {&sc[0], &sc[1], &sc[0]};
        P(nvnWindowBuilderSetTextures)(&wb2, 3, sc3); P(nvnWindowInitialize)(&w2, &wb2); P(nvnWindowSetPresentInterval)(&w2, 1);
        int ix;
        for (int k = 0; k < 3; k++) {
            P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
            P(nvnQueuePresentTexture)(&q, &w2, ix);
        }
        P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
        EXPECT("4th acquire returns texture 0", 0, ix);
        P(nvnQueuePresentTexture)(&q, &w2, ix);
        P(nvnWindowAcquireTexture)(&w2, NULL, &ix);
        EXPECT("5th acquire returns texture 1", 1, ix);
        nvn_present_config(0, 0);
    }

    // 6f. Present thread: when the drawable stays unavailable for a long time (the present thread stalls 200 ms each time), the present call still returns immediately;
    //     after the swapchain has gone a full round, re-acquiring texture 0 has to wait until its present has been processed (first enqueue + 200 ms)
    {
        nvn_present_config(0, 0);   // do not wait on the display link; only check that the present thread hands it back
        const double S = 200;
        mtl_present_test_stall((uint64_t)(S * 1e6));
        NVNwindowBuilder wb3; NVNwindow w3; P(nvnWindowBuilderSetDevice)(&wb3, &dev); P(nvnWindowBuilderSetDefaults)(&wb3);
        NVNtexture *sc3[3] = {&sc[0], &sc[1], &sc[0]};
        P(nvnWindowBuilderSetTextures)(&wb3, 3, sc3); P(nvnWindowInitialize)(&w3, &wb3);
        int ix;
        for (int k = 0; k < 3; k++) {
            P(nvnWindowAcquireTexture)(&w3, NULL, &ix);
            P(nvnQueuePresentTexture)(&q, &w3, ix);
        }
        P(nvnWindowAcquireTexture)(&w3, NULL, &ix);
        EXPECT("acquire returns texture 0", 0, ix);
        mtl_present_test_stall(0); mtl_present_flush();
    }

    // 7. Unimplemented functions: return 0, report a failure, report only once
    // Use a function that is still unimplemented (nvnBufferBuilderSetDefaults, used before, was implemented later and crashes on NULL)
    unsigned ui = 0; while (strcmp(nvn_names[ui], "nvnBufferBuilderGetDevice")) ui++;
    ((void (*)(void *))proc("nvnBufferBuilderGetDevice"))(NULL); ((void (*)(void *))proc("nvnBufferBuilderGetDevice"))(NULL);
    EXPECT("unimplemented function marked as reported ([RED]) after a call", 1, g_unimpl_seen[ui] > 0);
    EXPECT("unimplemented function count 2", 2, nvn_slot[ui].cnt);

    // 8. A texture buffer as 2D rows for wrapped 1D fetches (x & 0x3FFF, x >> 14; nvn_mtl.m tbuf_as_2d): full rows back to back, one short row rounded to the alignment
    {
        uint32_t w, h, row;
        EXPECT("256 R32 elements: one row", 1, nvn_tbuf_rows(256, 4, 64, &w, &h, &row) && w == 256 && h == 1 && row == 1024);
        EXPECT("10 R32 elements: one row rounded up to the 64-byte alignment", 1, nvn_tbuf_rows(10, 4, 64, &w, &h, &row) && w == 10 && h == 1 && row == 64);
        EXPECT("16384 RGBA32F elements: exactly one full row", 1, nvn_tbuf_rows(16384, 16, 256, &w, &h, &row) && w == 16384 && h == 1 && row == 262144);
        EXPECT("16385 R32 elements: two rows of 16384", 1, nvn_tbuf_rows(16385, 4, 64, &w, &h, &row) && w == 16384 && h == 2 && row == 65536);
        EXPECT("1,048,576 R32 elements: 64 rows", 1, nvn_tbuf_rows(1048576, 4, 64, &w, &h, &row) && w == 16384 && h == 64 && row == 65536);
        EXPECT("no elements: no rows", 0, nvn_tbuf_rows(0, 4, 64, &w, &h, &row));
        EXPECT("several rows whose alignment would break the wrapped address: refused", 0, nvn_tbuf_rows(20000, 4, 3, &w, &h, &row));
    }

    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
