// Exercise the real HLE, forced-takeover and stub indexes with synthetic Mach-O sections; no game files required.
#include "../hle.h"
#include <mach-o/getsect.h>
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static hle_ent entries[4];
static hle_force_ent forces[3];
static int have_sections;
static uint8_t *test_section(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; assert(!strcmp(seg, "__DATA"));
    *size = 0;
    if (!have_sections) return NULL;
    if (!strcmp(sect, "swhle")) { *size = sizeof entries; return (uint8_t *)entries; }
    assert(!strcmp(sect, "swforce")); *size = sizeof forces; return (uint8_t *)forces;
}
#define getsectiondata test_section
#include "../hle.c"
#include "../loader.c"
#undef getsectiondata

struct dm_flags g_flag;
uint64_t unimpl_stubs[MAX_STUB];
uint64_t now_ns(void) { return 0; }
uint64_t guest_ns(void) { return 0; }
void guest_clock_read_wait(void) {}
void hlecount_seal(void) {}
int svc_nro_deactivate(module_t *m) { m->loaded = 0; return 1; }   // no memory mappings in this fixture
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
static const char *wrap_kind, *wrap_name;
static unsigned wraps;
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    wrap_kind = kind; wrap_name = name; wraps++; return fn;
}

static void reset_sections(void) {
    free(g_hle_idx); g_hle_idx = NULL; g_hle_mask = 0; g_hle_sec = NULL;
    free(g_fidx); g_fidx = NULL; g_fmask = 0;
    free(g_force_hits); g_force_hits = NULL;
}

// Failures run in a child, so duplicate rejection and the hard stub limit are checked without killing the suite.
static void expect_abort(int duplicate) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        freopen("/dev/null", "w", stderr);
        if (duplicate) { forces[2] = forces[0]; force_init(); }
        else resolve_import(NULL, "one_more_missing_symbol");
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(void) {
    // Empty/missing sections must still give misses after lazy initialization.
    assert(!hle_implements("absent")); assert(!hle_implements("absent"));
    force_init(); assert(!force_lookup("absent")); reset_sections();

    // Three different names in the same initial bucket exercise probing and a duplicate behind a collision.
    char names[3][32]; unsigned found = 0;
    for (unsigned i = 0; found < 3; i++) {
        char candidate[32]; snprintf(candidate, sizeof candidate, "symbol_%u", i);
        if ((name_h(candidate) & 15) == 15) strcpy(names[found++], candidate);
    }
    int values[5];
    entries[0] = (hle_ent){names[0], &values[0]};
    entries[1] = (hle_ent){names[1], &values[1]};
    entries[2] = (hle_ent){names[1], &values[2]};
    entries[3] = (hle_ent){names[2], &values[3]};
    for (unsigned i = 0; i < 3; i++) forces[i] = (hle_force_ent){names[i], &values[i], "test"};
    have_sections = 1; force_init();
    for (unsigned i = 0; i < 3; i++) {
        unsigned before = wraps;
        assert(hle_implements(names[i]) && wraps == before);
        uint64_t want = (uint64_t)&values[i == 2 ? 3 : i];
        assert(resolve_import(NULL, names[i]) == want);
        assert(!strcmp(wrap_kind, "HLE") && !strcmp(wrap_name, names[i]));
        assert(force_lookup(names[i]) == &values[i]);
        assert(!strcmp(wrap_kind, "FORCE") && !strcmp(wrap_name, names[i]));
        assert(g_force_hits[i] == 1);
    }
    assert(!hle_implements("absent") && !force_lookup("absent") && !g_nstub);
    assert(force_lookup(names[0]) == &values[0] && g_force_hits[0] == 2);
    expect_abort(1);

    // Fill through every growth boundary, then query in reverse using different pointers to the same names.
    static char missing[MAX_STUB][32];
    for (unsigned i = 0; i < MAX_STUB; i++) {
        snprintf(missing[i], sizeof missing[i], "missing_%u", i);
        assert(resolve_import(NULL, missing[i]) == (uint64_t)&unimpl_stubs[i]);
        assert(!strcmp(wrap_kind, "STUB") && !strcmp(wrap_name, missing[i]));
    }
    for (unsigned i = MAX_STUB; i-- > 0;) {
        char copy[32]; strcpy(copy, missing[i]);
        assert(resolve_import(NULL, copy) == (uint64_t)&unimpl_stubs[i]);
    }
    assert(g_nstub == MAX_STUB); expect_abort(0);
    reset_sections();
    puts("Symbol indexes: collisions, duplicate precedence/rejection, misses, wrappers and 4096 reusable stubs: OK");
    return 0;
}
