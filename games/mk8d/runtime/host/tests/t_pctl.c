// pctl free communication offline policy (hle_misc.c, from PR #38): the hooks are looked up by their mangled names in the real swforce registrations, so a missing or misspelled
// takeover fails; no guest module, IPC session or system applet exists in this test
#include "../hle_misc.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop and error stand-ins hold the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>

static int unexpected, checks, fails;
void *guest_sym(const char *name) { (void)name; unexpected++; return NULL; }
module_t *module_of(const void *p) { (void)p; unexpected++; return NULL; }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; unexpected++; }
// The big-stack take-overs' stubs (HLE_FORCE_BIG) jump here with the implementation in x16; off a guest stack the real entry calls it straight
__asm__(".text\n.p2align 2\n.globl _dm_bigstack_enter\n_dm_bigstack_enter:\n    br x16\n");
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, name)) continue;
        count++; fn = (void *)e[i].fn;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
int main(void) {
    // bool arguments are widened as hle.h requires (only the low 8 bits count); the returns are bools in w0, not nn::Result
    unsigned (*check)(unsigned) = hook("_ZN2nn4pctl32CheckFreeCommunicationPermissionEb");
    unsigned (*begin)(unsigned) = hook("_ZN2nn4pctl25TryBeginFreeCommunicationEb");
    unsigned (*available)(void) = hook("_ZN2nn4pctl28IsFreeCommunicationAvailableEv");
    void (*end)(void) = hook("_ZN2nn4pctl20EndFreeCommunicationEv");
    unsigned (*stereo)(unsigned) = hook("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    const unsigned args[] = {0, 1, 0x100, 0x101, ~0u};
    if (available) CHECK(available() == 0, "communication unavailable before any permission query");
    for (size_t i = 0; i < sizeof args / sizeof *args; i++) {
        if (check) CHECK(check(args[i]) == 0, "permission query denies communication, with or without UI");
        if (begin) CHECK(begin(args[i]) == 0, "begin agrees with the permission query");
        if (stereo) CHECK(stereo(args[i]) == 1, "stereoscopic 3D stays allowed (positive control)");
        if (end) end();
        if (check) CHECK(check(args[i]) == 0, "ending communication does not grant permission");
        if (available) CHECK(available() == 0, "permission queries do not make communication available");
    }
    CHECK(unexpected == 0, "no guest symbol lookup or event binding");
    printf("pctl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
