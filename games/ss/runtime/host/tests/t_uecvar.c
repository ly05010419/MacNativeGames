// Unreal console variables named in main's fixups (dm_ue_cvar: the rewriter's ue_cvar.py finds them; dm/tools/tests/test_ue_cvar.py tests the search) and the texture pool raise
// (dm_ue_cvar_tick): the values the host raises, keeps and leaves, and a main without the variable
#include "../hle_uecvar.c"
#include <assert.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static module_t test_modules[1];
module_t *g_mod = test_modules;
int g_nmod;
static int writes;
void dm_guest_write_prepare(void *p, size_t n) { (void)p; assert(n == 4); writes++; }

enum { SIZE = 0x30000 };
static void cvar(const char *name, uint64_t off) { strcpy(g_mod[0].ue_cvar[g_mod[0].n_ue_cvar].name, name); g_mod[0].ue_cvar[g_mod[0].n_ue_cvar++].off = off; }

static void check(int which) {
    uint8_t *base = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); assert(base != MAP_FAILED);
    g_nmod = 1; strcpy(g_mod[0].name, "main"); g_mod[0].base = base; g_mod[0].size = SIZE;
    uint64_t obj = 0x205c8;
    cvar("t.MaxFPS", 0x20100);
    if (which != 1) cvar("r.Streaming.PoolSize", obj);                  // 1: the rewriter found no such variable (not registered, or registered twice)
    int32_t vals[2] = {200, 200};
    *(uintptr_t *)(base + obj + 0x10) = (uintptr_t)vals;
    setenv("DM_UE_TEXTURE_POOL", which == 2 ? "0" : "1000", 1);
    if (which == 1) { assert(!dm_ue_cvar("r.Streaming.PoolSize")); dm_ue_cvar_tick(); assert(vals[0] == 200 && writes == 0); return; }
    if (which == 2) { dm_ue_cvar_tick(); assert(vals[0] == 200 && vals[1] == 200 && writes == 0); return; }
    assert(dm_ue_cvar("r.Streaming.PoolSize") == obj && dm_ue_cvar("t.MaxFPS") == 0x20100);
    assert(!dm_ue_cvar("r.Streaming.LimitPoolSizeToVRAM"));            // not named
    dm_ue_cvar_tick(); assert(vals[0] == 1000 && vals[1] == 1000 && writes == 2);
    dm_ue_cvar_tick(); assert(writes == 2);                            // already raised
    vals[0] = vals[1] = 200; dm_ue_cvar_tick(); assert(vals[0] == 1000 && writes == 4);   // the game set its device profile again
    vals[0] = 2000; vals[1] = -1; dm_ue_cvar_tick(); assert(vals[0] == 2000 && vals[1] == -1 && writes == 4);   // a larger pool and no pool size stay
    munmap(base, SIZE);
}

int main(void) {
    // Each process starts the one-time lookup with fresh state
    for (int i = 0; i < 3; i++) {
        pid_t p = fork(); assert(p >= 0);
        if (!p) { check(i); _exit(0); }
        int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Unreal console variables: named by the rewriter, the texture pool raised, kept and left as set, a missing variable left alone");
}
