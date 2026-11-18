// A partly covered uniform shadow while an earlier submission still runs: that submission's inline updates are known to the CPU, so they do not refuse the shadow (the pass would end for it); its other GPU writes do.
// Splatoon Raiders updates 0x00..0x14 and 0x18..0x70 of its ink query block before each of about 150 queries a frame while the shader reads 0x00..0x70, and the page is updated again every frame
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { LEN = 0x20000 };
static uint8_t *mem;
static uint64_t running;   // the fence of the submission that stays unfinished

// What the open submission has queued goes into the running one
static void commit_as_running(void) { gw_commit(running); g_nubd = 0; memset(g_ubd_pf, 0, sizeof g_ubd_pf); g_ubd_wide = 0; pend_reset(); memset(g_ubpc, 0, sizeof g_ubpc); }
static void update(uint64_t off, uint32_t value, uint64_t n) { uint32_t v[16]; for (int i = 0; i < 16; i++) v[i] = value; mtl_sub_update(v, (uint64_t)mem + off, n); }
static const uint32_t *made_shadow(uint64_t off, uint64_t n) {
    uint64_t g; const uint8_t *c; if (!ubp_get((uint64_t)mem + off, (uint64_t)mem + off + n, &g, &c)) return NULL;
    ubc_wait(g_ubp_ready); return (const uint32_t *)c;
}
static void words(const uint32_t *got, const uint32_t *want, int n, const char *label) {
    CHECK(got && !memcmp(got, want, 4 * (size_t)n), label);
    if (got) for (int i = 0; i < n; i++) if (got[i] != want[i]) fprintf(stderr, "  word %d: %#x, want %#x\n", i, got[i], want[i]);
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR", "0", 1);
    mtl_init();
    mem = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    CHECK(mem != MAP_FAILED, "memory allocated"); if (mem == MAP_FAILED) return 1;
    mtl_pool_add(mem, LEN);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    running = g_done + 1000;
    memset(mem, 0x11, LEN);

    // The first case: every frame the same fields, with a hole (word 1) nothing updates
    update(0, 0xa0, 4); update(8, 0xa2, 4); commit_as_running();
    update(0, 0xb0, 4); update(8, 0xb2, 4);
    { const uint32_t want[] = {0xb0, 0x11111111, 0xb2}; words(made_shadow(0, 12), want, 3, "a hole beside fields updated by a running submission reads memory"); }
    commit_as_running();

    // A field only the running submission updated: memory does not hold it yet
    update(0x40, 0xc0, 4); commit_as_running();
    update(0x48, 0xc2, 4);
    { const uint32_t want[] = {0xc0, 0x11111111, 0xc2}; words(made_shadow(0x40, 12), want, 3, "a running submission's update shows through"); }
    commit_as_running();

    // Newest wins across running submissions and a page boundary
    update(0xff8, 0xd0, 16); commit_as_running();
    update(0xffc, 0xd1, 8); commit_as_running();
    update(0xff8, 0xd2, 4);
    { const uint32_t want[] = {0xd2, 0xd1, 0xd1, 0xd0}; words(made_shadow(0xff8, 16), want, 4, "several running updates in order, over a page boundary"); }
    commit_as_running();

    // A running copy or fill the CPU cannot see refuses, and only on its page
    pend_add((uint64_t)mem + 0x8000, 16, 3); commit_as_running();
    update(0x8010, 0xe0, 4);
    CHECK(!made_shadow(0x8000, 32), "a running fill refuses a shadow of its page");
    update(0x10000, 0xe1, 4);
    CHECK(made_shadow(0x10000, 32) != NULL, "and not of another page");
    commit_as_running();

    // Once it completes, memory holds the truth: what the CPU wrote after it is not overwritten by what was remembered
    g_done = running; running += 1000;
    memset(mem, 0x22, LEN);
    update(0x48, 0xf2, 4);
    { const uint32_t want[] = {0x22222222, 0x22222222, 0xf2}; words(made_shadow(0x40, 12), want, 3, "a finished update is no longer laid over memory"); }
    CHECK(made_shadow(0x8000, 32) == NULL ? (update(0x8010, 0xf0, 4), made_shadow(0x8000, 32) != NULL) : 1, "a finished fill no longer refuses");
    commit_as_running();

    // A running update on a page that shares its table slot with another running page: refused rather than lost
    uint64_t other = 0;
    for (uint64_t off = 0x1000; off < LEN && !other; off += 0x1000) if (off != 0x18000 && um_slot(((uint64_t)mem + off) >> 12) == um_slot(((uint64_t)mem + 0x18000) >> 12)) other = off;
    if (other) { update(0x18000, 0xa5, 4); update(other, 0xa6, 4); commit_as_running();
        update(other + 8, 0xa7, 4);
        const uint32_t *got = made_shadow(other, 12);
        CHECK(!got || got[0] == 0xa6, "a slot collision refuses or shows the update"); commit_as_running(); }

    pend_reset(); g_nubd = 0;
    printf("t_ubp_running: %d checks, %d failed\n", checks, failed); fflush(stdout);
    _exit(failed != 0);
}}
