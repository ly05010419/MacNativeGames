#pragma once
#include <stddef.h>
#include <stdint.h>

/* Atmosphère cheat programs (the dmnt cheat VM format the Switch scene and Yuzu/Ryujinx use: "[Name]" sections of 32-bit
   hex words, "{Master}" sections that always run). Game-independent: the runtime glue (cheats.c) supplies the memory
   region bases, guarded memory access and the held buttons. */
enum { CHEAT_MAX = 128, CHEAT_MAX_WORDS = 0x400, CHEAT_NAME = 96 };
typedef struct { char name[CHEAT_NAME]; int master; unsigned n; uint32_t w[CHEAT_MAX_WORDS]; } Cheat;

/* Parses a cheat file's text into out (at most max). Returns the count, or -1 with err set. Sections without any
   instruction words (titles, notes) are kept with n = 0. */
int cheat_parse(const char *text, Cheat *out, int max, char *err, size_t errn);
/* Instruction length in words starting at w[i] (0: unknown or truncated). */
unsigned cheat_insn_len(const uint32_t *w, unsigned n, unsigned i);

/* Memory access types: 0 main module, 1 heap, 2 alias region, 3 ASLR region. */
typedef struct {
    uint64_t base[4];
    uint64_t keys;   /* held buttons, HID npad bit order (A = bit 0, B, X, Y, ...) */
    void *ctx;
    int (*read)(void *ctx, uint64_t addr, void *out, unsigned size);         /* 0: not readable */
    int (*write)(void *ctx, uint64_t addr, const void *in, unsigned size);   /* 0: refused */
} CheatEnv;
/* Runs one cheat program once. Returns 1, or 0 when it stopped early (failed read, refused write, malformed). */
int cheat_run(const Cheat *c, const CheatEnv *env, char *err, size_t errn);
