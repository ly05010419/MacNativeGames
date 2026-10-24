/* Unit test for runtime/cheat_vm.c (the Atmosphère cheat format): parsing, instruction lengths, pointer chains, static
   and register stores, button conditions, register conditionals with else, loops, arithmetic, and the failure paths
   (unreadable pointer: silent; refused write and malformed program: reported). Memory is a local buffer whose first
   0x100 bytes stand for read-only code. A negative control checks that a wrong chain offset writes nothing.
   Prints one JSON line; exit 1 on failure. */
#include "cheat_vm.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static uint8_t mem[0x10000] __attribute__((aligned(16)));
static int writes;
static int in_mem(uint64_t a, unsigned n) { uint64_t b = (uintptr_t)mem; return a >= b && a - b + n <= sizeof mem; }
static int rd(void *ctx, uint64_t a, void *out, unsigned n) { (void)ctx; if (!in_mem(a, n)) return 0; memcpy(out, (void *)(uintptr_t)a, n); return 1; }
static int wr(void *ctx, uint64_t a, const void *in, unsigned n) {
    (void)ctx;
    if (!in_mem(a, n) || a - (uintptr_t)mem < 0x100) return 0;   // "code" is not writable
    memcpy((void *)(uintptr_t)a, in, n); writes++; return 1;
}
static void put64(unsigned off, uint64_t v) { memcpy(mem + off, &v, 8); }
static void put32(unsigned off, uint32_t v) { memcpy(mem + off, &v, 4); }
static uint32_t get32(unsigned off) { uint32_t v; memcpy(&v, mem + off, 4); return v; }
static uint64_t get64(unsigned off) { uint64_t v; memcpy(&v, mem + off, 8); return v; }

static Cheat cs[8];
static int run_saved(const char *text, uint64_t keys, uint64_t *saved, char *err, size_t errn) {
    char perr[128];
    int n = cheat_parse(text, cs, 8, perr, sizeof perr);
    if (n != 1) { snprintf(err, errn, "parse: %d %s", n, n < 0 ? perr : ""); return -1; }
    CheatEnv env = {.base = {(uintptr_t)mem, (uintptr_t)mem + 0x8000, 0, 0}, .keys = keys, .read = rd, .write = wr, .saved = saved};
    err[0] = 0;
    return cheat_run(&cs[0], &env, err, errn);
}
static int run_text(const char *text, uint64_t keys, char *err, size_t errn) { return run_saved(text, keys, NULL, err, errn); }

int main(void) {
    char err[160];
    /* Parsing: titles and notes keep n = 0, {master} is marked, words are collected per section. */
    static Cheat p[8];
    int n = cheat_parse("[Game v0 BID=0123]\n\n[One]\n04000000 00000200 0000002A\n{Master}\n20000000\n[-NOTE- x]\n", p, 8, err, sizeof err);
    CHECK(n == 4, "parsed %d sections (want 4)", n);
    CHECK(n == 4 && p[0].n == 0 && p[1].n == 3 && p[2].master && p[2].n == 1 && p[3].n == 0, "section word counts / master flag");
    CHECK(n == 4 && !strcmp(p[1].name, "One") && !strcmp(p[3].name, "-NOTE- x"), "section names");
    CHECK(cheat_parse("[X]\n0400000 00000200\n", p, 8, err, sizeof err) < 0, "a 7-digit word is rejected");
    CHECK(cheat_parse("04000000\n", p, 8, err, sizeof err) < 0, "words before the first section are rejected");

    /* Instruction lengths. */
    uint32_t l[] = {0x08000000, 0, 0, 0, 0x580F1000, 0, 0x640F1000, 0, 0, 0x989BF000, 0xC046B400, 0xFFFFFFFF, 0xA4FA0200, 8, 0x80000004};
    unsigned want[] = {4, 0, 0, 0, 2, 0, 3, 0, 0, 1, 2, 0, 2, 0, 1};
    for (unsigned i = 0; i < sizeof l / 4; i++) if (want[i]) CHECK(cheat_insn_len(l, sizeof l / 4, i) == want[i], "length of %08x: %u", l[i], cheat_insn_len(l, sizeof l / 4, i));
    CHECK(cheat_insn_len(l, 2, 0) == 0, "a truncated instruction has length 0");

    /* Pointer chain: [main+0x400] -> A; [A+0x10] -> B; store 998 at B (type 6 with increment). */
    put64(0x400, (uintptr_t)mem + 0x1000); put64(0x1010, (uintptr_t)mem + 0x2000); put32(0x2000, 5);
    int r = run_text("[Arrows]\n580F0000 00000400\n580F1000 00000010\n780F0000 00000000\n640F1000 00000000 000003E6\n", 0, err, sizeof err);
    CHECK(r == 1 && get32(0x2000) == 998, "pointer chain store: run %d, value %u (%s)", r, get32(0x2000), err);
    /* Negative control: a wrong first offset leads to an unreadable pointer, so nothing is written and no error is reported. */
    put32(0x2000, 5); writes = 0;
    r = run_text("[Arrows]\n580F0000 00000408\n580F1000 00000010\n640F1000 00000000 000003E6\n", 0, err, sizeof err);
    CHECK(r == 0 && writes == 0 && get32(0x2000) == 5 && !err[0], "wrong offset: run %d writes %d value %u err \"%s\"", r, writes, get32(0x2000), err);

    /* Button condition: write only while X (bit 2) is held; heap base (memory type 1) + 0x10. */
    put32(0x8010, 0);
    const char *jump = "[Jump]\n80000004\n04100000 00000010 41200000\n20000000\n";
    r = run_text(jump, 0x1, err, sizeof err);
    CHECK(r == 1 && get32(0x8010) == 0, "button not held: no write (%u)", get32(0x8010));
    r = run_text(jump, 0x4 | 0x1, err, sizeof err);
    CHECK(r == 1 && get32(0x8010) == 0x41200000, "button held: heap write (%08x)", get32(0x8010));

    /* Loop over 4 slots of 8 bytes: slot value != -1 -> store 98 (and step 4), else step 8 (the "98 of everything" shape). */
    for (unsigned i = 0; i < 4; i++) { put32(0x3000 + 8 * i, i == 2 ? 0xFFFFFFFF : 1); put32(0x3004 + 8 * i, 0); }
    put64(0x500, (uintptr_t)mem + 0x3000);
    r = run_text("[Fill]\n580F0000 00000500\n300C0000 00000004\n989BF000\n540B1000 00000000\nC046B400 FFFFFFFF\n640F1000 00000000 00000062\n"
                 "780F0000 00000004\n21000000\n780F0000 00000008\n20000000\n310C0000\n", 0, err, sizeof err);
    CHECK(r == 1 && get32(0x3000) == 98 && get32(0x3008) == 98 && get32(0x3010) == 0xFFFFFFFF && get32(0x3018) == 98,
          "loop/else: run %d slots %u %u %08x %u (%s)", r, get32(0x3000), get32(0x3008), get32(0x3010), get32(0x3018), err);

    /* Register store with offset (type A, O=2) and arithmetic (type 9 immediate add, register move). */
    put64(0x600, (uintptr_t)mem + 0x4000); put32(0x4000, 0);
    r = run_text("[Store]\n580A0000 00000600\n400F0000 00000000 00000005\n940FF100 00000025\nA4FA0200 00000008\n", 0, err, sizeof err);
    CHECK(r == 1 && get32(0x4008) == 0x2A, "type A store of 5+0x25: %08x (%s)", get32(0x4008), err);
    r = run_text("[Mov]\n580A0000 00000600\n400F0000 00000001 00000002\n9899F000\nA89A0000\n", 0, err, sizeof err);
    CHECK(r == 1 && get64(0x4000) == 0x100000002ull, "register move and 64-bit store: %016llx (%s)", (unsigned long long)get64(0x4000), err);

    /* Saved values (C1) carry over from one run to the next when the caller keeps them, and start at zero when it
       does not: restore R0, add 1, save it, store it. */
    const char *count = "[Count]\n580A0000 00000600\nC1000000\n78000000 00000001\nC1000010\nA40A0200 00000008\n";
    uint64_t keep[16] = {0};
    for (int k = 0; k < 3; k++) r = run_saved(count, 0, keep, err, sizeof err);
    CHECK(r == 1 && get32(0x4008) == 3 && keep[0] == 3, "kept saved values count runs: %u (%s)", get32(0x4008), err);
    for (int k = 0; k < 3; k++) r = run_text(count, 0, err, sizeof err);
    CHECK(r == 1 && get32(0x4008) == 1, "without kept values each run starts at zero: %u (%s)", get32(0x4008), err);

    /* Static store into the main module: allowed above the "code", refused inside it (reported). */
    r = run_text("[Data]\n04000000 00000700 12345678\n", 0, err, sizeof err);
    CHECK(r == 1 && get32(0x700) == 0x12345678, "static main store (%s)", err);
    r = run_text("[Code]\n04000000 00000010 D503201F\n", 0, err, sizeof err);
    CHECK(r == 0 && err[0] && get32(0x10) == 0, "write into code refused and reported: run %d err \"%s\"", r, err);
    /* Malformed: unknown opcode type B, a loop end without a start, an unknown arithmetic operation. */
    r = run_text("[Bad]\nB0000000\n", 0, err, sizeof err);
    CHECK(r == 0 && err[0], "unknown opcode reported (%s)", err);
    r = run_text("[Bad]\n310C0000\n", 0, err, sizeof err);
    CHECK(r == 0 && err[0], "loop end without start reported (%s)", err);
    r = run_text("[Bad]\n98F0F100 00000001\n", 0, err, sizeof err);
    CHECK(r == 0 && err[0], "unknown arithmetic reported (%s)", err);

    printf("{\"status\":\"%s\",\"probe\":\"probe-cheat-vm\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checks, failures);
    return failures ? 1 : 0;
}
