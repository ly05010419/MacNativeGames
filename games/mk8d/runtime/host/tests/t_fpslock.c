// Exercise production build gates and frame-limit writes against sparse synthetic module memory.
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
void dm_guest_write_prepare(void *p, size_t n) { (void)p; assert(n == 4); writes++; }

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
    } else if (which == 8) {
        strcpy(g_mod[0].build_id, "9fe60a212cff9b9a3656ab63d4f882e5");
        *(uintptr_t *)(base + 0x7c738d0 + 0x10) = (uintptr_t)object;
        *(uintptr_t *)(base + 0x7b32690 + 0x10) = (uintptr_t)(object + 16);
        f = (float *)object; f[0] = f[1] = 30;
        sync = (int32_t *)(object + 16); sync[0] = sync[1] = 2;
    } else if (which >= 9) {
        strcpy(g_mod[0].build_id, "b7f965b9d7227db8e29f04e84ea7331b");
        if (which == 12) g_mod[0].build_id[31] = '0';
        *(uintptr_t *)(base + 0x8adca10 + 0x10) = which == 13 ? 0 : (uintptr_t)object;
        sync = (int32_t *)object; sync[0] = sync[1] = which == 10 ? 1 : 2;
    }
    dm_fps_lock_tick();
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
    for (int i = 0; i < 14; i++) {
        pid_t p = fork(); assert(p >= 0);
        if (!p) { check(i); _exit(0); }
        int status; assert(waitpid(p, &status, 0) == p); assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    puts("Frame limits: integer/float FPS, milliseconds, vsync copies, build gates and overrides passed");
}
