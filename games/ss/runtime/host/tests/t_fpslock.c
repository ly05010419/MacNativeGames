// Exercise production build gates and frame-limit writes against sparse synthetic module memory; Unreal's rows come from the console variables the rewriter names in main's fixups.
#include "../hle_fpslock.c"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;
static int writes;
void dm_guest_write_prepare(void *p, size_t n) { (void)p; assert(n == 4 || n == 8); writes++; }

// adrp / add of a module offset into register r, at word i of the text (module offsets: the text at 0)
static void cvar(const char *name, uint64_t off) { strcpy(g_mod[0].ue_cvar[g_mod[0].n_ue_cvar].name, name); g_mod[0].ue_cvar[g_mod[0].n_ue_cvar++].off = off; }

static void adrp_add(uint32_t *t, uint32_t i, int r, uint64_t target) {
    int64_t imm = (int64_t)((target & ~0xfffull) - ((i * 4ull) & ~0xfffull)) >> 12;
    t[i] = 0x90000000u | (uint32_t)(imm & 3) << 29 | (uint32_t)((imm >> 2) & 0x7ffff) << 5 | (uint32_t)r;
    t[i + 1] = 0x91000000u | (uint32_t)(target & 0xfff) << 10 | (uint32_t)r << 5 | (uint32_t)r;
}
// A Frostbite main in miniature: the class name and field names in read-only data, the class record and its field list, the static constructor's registration of the
// type info and a reader's settings lookup in the text, the instance cache word and the instance
static uint8_t *frostbite(uint8_t *base, uint8_t *inst) {
    g_mod[0].text_end = 0x1000; g_mod[0].ro_off = 0x2000; g_mod[0].size = 0x13000000;
    uint64_t b = (uint64_t)(uintptr_t)base, rec = 0x3008, fields = 0x3100, ti = 0x40000, cache = 0x41000;
    static const char names[] = "\0SimulationTimeSettingsX\0SimulationTimeSettings\0ForceSimRate\0MaxSimFps";
    memcpy(base + 0x2000, names, sizeof names);
    *(uint64_t *)(base + 0x3000) = b + 0x2001;   // a record of another class whose name starts the same
    *(uint64_t *)(base + rec) = b + 0x2019; *(uint64_t *)(base + rec + 0x40) = b + fields;
    *(uint64_t *)(base + fields) = b + 0x203d; *(uint32_t *)(base + fields + 0x10) = 0x34;
    *(uint64_t *)(base + fields + 0x20) = b + 0x2030; *(uint32_t *)(base + fields + 0x30) = 0x38;
    uint32_t *t = (uint32_t *)base;
    adrp_add(t, 100, 19, ti); adrp_add(t, 102, 8, rec); t[104] = 0xF9000000u | 19 << 5 | 8;   // str x8, [x19]
    adrp_add(t, 200, 2, 0x42000);   // another word in x2 well before the lookup
    adrp_add(t, 220, 1, ti); adrp_add(t, 222, 2, cache); t[224] = 0x94000000u;   // bl
    *(uint64_t *)(inst + 8) = b + ti; *(uint64_t *)(base + cache) = (uint64_t)(uintptr_t)inst;
    return inst;
}

static void check(int which) {
    size_t len = 0x13000000;
    uint8_t *base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base;
    strcpy(g_mod[0].build_id, "94ceae325c205c4b9d6f7235552f28fd");
    setenv("DM_FPS_TARGET", which == 1 || which == 10 ? "30" : which == 2 || which == 11 ? "0" : "60", 1);
    int32_t *unity = (int32_t *)(base + 0x4afc868);
    *unity = which == 1 ? 60 : which == 4 ? -1 : which == 5 ? 0 : 30;
    uint8_t object[0x500] = {0};
    float *f = NULL; int32_t *sync = NULL;
    if (which == 3) g_mod[0].build_id[0] = '0';
    if (which == 6) {
        strcpy(g_mod[0].build_id, "c9a3dd7702075ecd90101f4555389582");
        *(uintptr_t *)(base + 0x1295b3f0) = (uintptr_t)object;
        f = (float *)(object + 0x434); *f = 30;
    } else if (which == 7) {
        strcpy(g_mod[0].build_id, "79e5950ffa85acf63e28c9aec051eac2");
        f = (float *)(base + 0x13c94dc); *f = 1000.0f / 30;
    } else if (which == 8) {   // Octopath Traveler 0: the frame limit and the present interval, and a variable the host does not write
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        cvar("r.Streaming.PoolSize", 0x7c71608); cvar(DM_GAME_UE_FPS_CVAR, 0x7c738d0); cvar("rhi.SyncInterval", 0x7b32690);
        *(uintptr_t *)(base + 0x7c738d0 + 0x10) = (uintptr_t)object;
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        f = (float *)object; f[0] = f[1] = 30;
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
    } else if (which == 14) {
        strcpy(g_mod[0].build_id, "a48c0de3bc353efa0e4d359bde05ef24");
        *(uintptr_t *)(base + 0xda20e38) = (uintptr_t)object;
        double *d = (double *)(object + 8); d[0] = d[1] = 30;
    } else if (which == 15 || which == 16) {
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        frostbite(base, object);
        *(float *)(object + 0x34) = 30; *(int32_t *)(object + 0x38) = 30;
        if (which == 16) *(uint64_t *)(object + 8) = 0;   // the cache word holds something else
    } else if (which >= 9) {   // the present interval alone (UE 4.18's nvn.SyncInterval); 12: a main the rewriter found no variable in
        strcpy(g_mod[0].build_id, "0123456789abcdef0123456789abcdef");
        if (which != 12) cvar("nvn.SyncInterval", 0x8adca10);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = which == 13 ? 0 : (uintptr_t)object;
        sync = (int32_t *)object; sync[0] = sync[1] = which == 10 ? 1 : 2;
    }
    int engine = which != 3 && which != 12;   // a limiter the host writes: any but another build's main (3) and a main the rewriter found no Unreal variable in (12)
    if (which & 1) assert(dm_fps_lock_engine() == engine);   // the graphics layer's present cap may ask before the first present (the acquire)
    dm_fps_lock_tick();
    assert(dm_fps_lock_engine() == engine);
    if (which == 0) {
        assert(*unity == 60 && writes == 1);
        dm_fps_lock_tick(); assert(writes == 1);
        *unity = 30; dm_fps_lock_tick(); assert(*unity == 60 && writes == 2);
    } else if (which == 1) assert(*unity == 30 && writes == 1);
    else if (which >= 2 && which <= 5) {
        assert(writes == 0);
        assert(*unity == (which == 4 ? -1 : which == 5 ? 0 : 30));
    } else if (which == 6) assert(*f == 60 && writes == 1);
    else if (which == 7) assert(fabsf(*f - (1000.0f / 60 - 0.5f)) < 0.001f && writes == 1);
    else if (which == 8) assert(f[0] == 60 && f[1] == 60 && sync[0] == 1 && sync[1] == 1 && writes == 4);
    else if (which == 9) {
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        sync[0] = sync[1] = 2; dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 4);
    } else if (which == 10) assert(sync[0] == 2 && sync[1] == 2 && writes == 2);
    else if (which == 14) { double *d = (double *)(object + 8); assert(d[0] == 60 && d[1] == 60 && writes == 2); }
    else if (which == 15) {
        assert(*(float *)(object + 0x34) == 60 && *(int32_t *)(object + 0x38) == 60 && writes == 2);
        dm_fps_lock_tick(); assert(writes == 2);
        *(int32_t *)(object + 0x38) = 30; dm_fps_lock_tick(); assert(*(int32_t *)(object + 0x38) == 60 && writes == 3);
    } else if (which == 16) assert(*(float *)(object + 0x34) == 30 && *(int32_t *)(object + 0x38) == 30 && writes == 0);
    else if (which == 13) {
        assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = (uintptr_t)object;
        dm_fps_lock_tick();
        assert(sync[0] == 1 && sync[1] == 1 && writes == 2);
    }
    else assert(sync[0] == 2 && sync[1] == 2 && writes == 0);
    munmap(base, len);
}

int main(void) {
    // Each process starts the production one-time initialization with a fresh build and target.
    for (int i = 0; i < 17; i++) {
        pid_t p = fork(); assert(p >= 0);
        if (!p) { check(i); _exit(0); }
        int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Frame limits: integer/float FPS, milliseconds, vsync copies, build gates and overrides passed");
}
