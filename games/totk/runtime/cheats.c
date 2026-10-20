/* Atmosphère-format cheats for the main module (Mac).

   File: $TOTK_CHEATS_DIR/<build id>.txt, where <build id> is the first 16 hex digits of the main module's build ID, the
   name cheat databases use (1.4.3: 277178B7DBA1B6D4). The launcher points TOTK_CHEATS_DIR at
   ~/Library/Application Support/MacNativeGames/TOTK/cheats; TOTK_CHEATS=0 turns cheats off. The build ID is read from the
   loaded image's GNU build-id note, so a file only ever loads for the build its name says.

   Format: "[Name]" or "{Name}" starts a section, followed by lines of 8-digit hex words. Every section is applied, except
   one whose header is written "#[Name]" (disabled until the next header). Other lines starting with '#' or "//" are
   comments. A section holding only "20000000" / "20000001" words is a cheat-manager folder marker and is ignored.

   Static sections (only opcode 0: "0TMR00AA AAAAAAAA VVVVVVVV [VVVVVVVV]", memory type 0 = main) are applied once,
   before the text is sealed read-only, at the same point as the game patches: the code stays changed for the session.
   Writes into the data segment also happen once; the game may overwrite them later.

   Button sections (opcode 8 "8kkkkkkk" key held, 21000000 else, 20000000 end, around opcode-0 writes) are run every time
   the game reads the controller (runtime_cheats_input), like Atmosphère's cheat VM. Their writes may target
     - read-only or writable data: written directly (a read-only page becomes writable after sealing);
     - code, only where every value is a MOVZ / MOVN of one register (e.g. "mov w8, #1.0f"): the instruction is replaced
       once by a branch to a small veneer that loads the register from a slot on the main module's private data page,
       and the slot is updated instead. Other code edits cannot change while the code is sealed: such sections are skipped.
   Key bits are the console's HidNpadButton bits (A 1, B 2, X 4, Y 8, L3 10, R3 20, L 40, R 80, ZL 100, ZR 200, + 400,
   - 800, D-pad left/up/right/down 1000/2000/4000/8000, left stick left/up/right/down 10000/20000/40000/80000, right stick
   100000/200000/400000/800000); stick directions are derived from the stick position (more than half way).

   Any other opcode (registers, pointer chains, loops) is not supported: its whole section is skipped. A section is also
   skipped if any write falls outside the game's own image or would overwrite an instruction this port rewrote (a branch
   into the patch area). A skipped section writes nothing; the game still starts.

   The cheat format carries no original instruction, so a code made for another game version cannot be detected here
   beyond the build ID in the file name. Every write is logged with the word it replaces ("[cheats]" lines in totk.log). */
#include "runtime.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define CHEAT_MAX_WORDS 4096
#define CHEAT_MAX_FILE (1u << 20)
#define LIVE_MAX_OPS 2048
#define LIVE_MAX_SECTIONS 64
#define LIVE_MAX_SLOTS 64
#define LIVE_MAX_DEPTH 8
#define SLOT_AREA 0x1000u     /* slots: private data page + 0x1000 (the game patches use its first bytes) */
#define VENEER_AREA 0x400u    /* veneers: the last 1 KiB before that page, inside the executable patch area */

enum { OP_WRITE, OP_KEY, OP_ELSE, OP_END };
typedef struct { uint8_t op, width; int16_t slot; uint32_t keys; uint64_t off, value; } CheatOp;
typedef struct { char name[96]; unsigned first, count, active_logged; } LiveSection;
typedef struct { uint64_t off; uint32_t rd, sf; } Slot;

static Module *live_module;
static CheatOp live_ops[LIVE_MAX_OPS];
static unsigned live_nops;
static LiveSection live_sections[LIVE_MAX_SECTIONS];
static unsigned live_nsections;
static Slot slots[LIVE_MAX_SLOTS];
static unsigned nslots;
static uint64_t ro_pages[LIVE_MAX_OPS];
static unsigned nro_pages;

static uint64_t page_up16k(uint64_t v) { return (v + 0x3fff) & ~0x3fffull; }
static uint64_t private_page(const Module *m) { return page_up16k(m->file_size); }

static int slot_index(uint64_t off) { for (unsigned i = 0; i < nslots; i++) if (slots[i].off == off) return (int)i; return -1; }

/* The NSO build ID is the ELF GNU build-id note (namesz 4, type 3, "GNU"), kept in the module's read-only data. Its
   descsz is 20 in Tears of the Kingdom and 16 in Breath of the Wild; cheat files use the first 8 bytes either way. */
static int main_build_id(const Module *m, char out[17]) {
    static const uint8_t note[16] = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
    uint64_t lo = m->text_end, hi = m->key_off;
    if (hi > m->file_size) hi = m->file_size;
    for (uint64_t p = lo & ~3ull; p + sizeof note + 8 <= hi; p += 4) {
        const uint8_t *q = m->base + p;
        if (memcmp(q, note, 4) || (q[4] != 20 && q[4] != 16) || memcmp(q + 5, note + 5, sizeof note - 5)) continue;
        for (unsigned i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02X", m->base[p + sizeof note + i]);
        return 1;
    }
    return 0;
}

static int is_hex_word(const char *s, size_t n) {
    if (n != 8) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

/* A rewritten site holds B/BL to the port's patch area; original game code never branches there. */
static int rewritten_at(const Module *m, uint64_t off) {
    if (off + 4 > m->text_end || slot_index(off) >= 0) return 0;   /* a button cheat's own veneer branch is not the port's */
    uint32_t w; memcpy(&w, m->base + off, 4);
    if ((w & 0x7c000000u) != 0x14000000u) return 0;
    int64_t imm = w & 0x3ffffff; if (imm & 0x2000000) imm -= 0x4000000;
    return (int64_t)off + imm * 4 >= (int64_t)m->patch_off;
}

/* MOVZ / MOVN (32 or 64 bit) into a general register other than xzr: returns 1 and the register, width and value. */
static int mov_imm(uint32_t w, uint32_t *rd, uint32_t *sf, uint64_t *value) {
    if (((w >> 23) & 0x3f) != 0x25) return 0;
    uint32_t opc = (w >> 29) & 3; if (opc != 0 && opc != 2) return 0;
    *sf = w >> 31; *rd = w & 31; if (*rd == 31) return 0;
    uint32_t hw = (w >> 21) & 3; if (!*sf && hw > 1) return 0;
    uint64_t v = (uint64_t)((w >> 5) & 0xffff) << (16 * hw);
    if (opc == 0) v = ~v;
    if (!*sf) v &= 0xffffffffull;
    *value = v;
    return 1;
}

static const char *opcode_name(unsigned type) {
    switch (type) {
    case 1: return "a memory condition";
    case 3: return "a loop";
    case 4: case 5: case 6: case 7: case 9: case 10: return "a register or pointer operation";
    default: return "an extended opcode";
    }
}

static const char *region(const Module *m, uint64_t off) {
    return off < m->text_end ? "text" : off < page_up16k(m->text_end) ? "text padding" : off < m->data_off ? "rodata"
         : "data, written once: the game may change it later";
}

/* Decode a section. Returns NULL when it can be used (kind: 0 static, 1 button, 2 folder marker), otherwise why not. */
static const char *decode(const Module *m, const uint32_t *w, unsigned n, CheatOp *out, unsigned *nout, int *kind, char *why, size_t why_len) {
    unsigned depth = 0, writes = 0, keys = 0;
    *nout = 0;
    for (unsigned i = 0; i < n;) {
        unsigned type = w[i] >> 28;
        if (*nout >= CHEAT_MAX_WORDS) { snprintf(why, why_len, "too many codes"); return why; }
        if (type == 8) { out[(*nout)++] = (CheatOp){.op = OP_KEY, .keys = w[i] & 0x0fffffff}; depth++; keys++; i++;
            if (depth > LIVE_MAX_DEPTH) { snprintf(why, why_len, "button conditions nested too deep"); return why; } continue; }
        if (type == 2) {
            unsigned sub = (w[i] >> 24) & 0xf;
            if (sub > 1) { snprintf(why, why_len, "unknown end opcode %08X", w[i]); return why; }
            out[(*nout)++] = (CheatOp){.op = sub ? OP_ELSE : OP_END};
            if (!sub && depth) depth--;
            i++; continue;
        }
        if (type != 0) { snprintf(why, why_len, "it uses %s (opcode %X), which this loader does not run", opcode_name(type), type); return why; }
        unsigned width = (w[i] >> 24) & 0xf, mem = (w[i] >> 20) & 0xf;
        if (width != 1 && width != 2 && width != 4 && width != 8) { snprintf(why, why_len, "write width %u is not 1, 2, 4 or 8", width); return why; }
        if (mem != 0) { snprintf(why, why_len, "it writes to memory type %u; only the main module (type 0) is supported", mem); return why; }
        unsigned need = width == 8 ? 4 : 3;
        if (i + need > n) { snprintf(why, why_len, "the last code is cut short"); return why; }
        uint64_t off = (uint64_t)(w[i] & 0xff) << 32 | w[i + 1];
        uint64_t value = width == 8 ? (uint64_t)w[i + 2] << 32 | w[i + 3] : (uint64_t)w[i + 2];
        if (width < 8) value &= (1ull << (8 * width)) - 1;
        if (off + width > m->key_off) { snprintf(why, why_len, "main+0x%llx is outside the game's image (ends at 0x%llx)", (unsigned long long)off, (unsigned long long)m->key_off); return why; }
        for (uint64_t a = off & ~3ull; a < off + width; a += 4)
            if (rewritten_at(m, a)) { snprintf(why, why_len, "main+0x%llx is an instruction this port rewrote", (unsigned long long)a); return why; }
        out[(*nout)++] = (CheatOp){.op = OP_WRITE, .width = (uint8_t)width, .slot = -1, .off = off, .value = value};
        writes++; i += need;
    }
    if (!writes && !keys) { *kind = 2; return NULL; }
    if (!keys) {
        for (unsigned i = 0; i < *nout; i++) if (out[i].op != OP_WRITE) { snprintf(why, why_len, "an end / else without a button condition"); return why; }
        *kind = 0; return NULL;
    }
    *kind = 1;
    /* Button section: every code write must be a MOVZ / MOVN of one register, as must the original instruction there. */
    for (unsigned i = 0; i < *nout; i++) {
        const CheatOp *o = &out[i];
        if (o->op != OP_WRITE || o->off >= page_up16k(m->text_end)) continue;
        uint32_t rd, sf, rd0 = 0, sf0 = 0, orig; uint64_t v; int have = slot_index(o->off);
        memcpy(&orig, m->base + (o->off & ~3ull), 4);
        int orig_ok = have >= 0 ? (rd0 = slots[have].rd, sf0 = slots[have].sf, 1) : mov_imm(orig, &rd0, &sf0, &v);   /* a site another section already switched keeps its original register */
        if (o->width != 4 || (o->off & 3) || !mov_imm((uint32_t)o->value, &rd, &sf, &v) || !orig_ok || rd != rd0 || sf != sf0) {
            snprintf(why, why_len, "it changes code at main+0x%llx while playing; only MOVZ/MOVN register values can switch with a button", (unsigned long long)o->off);
            return why;
        }
    }
    return NULL;
}

/* ---- live (button) sections ---- */

static uint32_t enc_b(uint64_t pc, uint64_t target) {
    int64_t d = (int64_t)target - (int64_t)pc;
    if ((d & 3) || d < -(1ll << 27) || d >= (1ll << 27)) return 0;
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}
static uint32_t enc_adrp(uint32_t rd, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | rd;
}

/* The code site gets "b veneer"; the veneer is "adrp xR, page; ldr wR/xR, [xR, #slot]; b site+4". */
static int slot_for(Module *m, uint64_t off, char *why, size_t why_len) {
    int have = slot_index(off); if (have >= 0) return have;
    if (nslots >= LIVE_MAX_SLOTS) { snprintf(why, why_len, "too many button-controlled code sites"); return -1; }
    uint64_t page = private_page(m), stubs = (m->file_size + 15) & ~15ull;
    if (page + 0x4000 != m->size || page - VENEER_AREA < stubs + 64) { snprintf(why, why_len, "no room for a code veneer in this module layout"); return -1; }
    uint64_t veneer = page - VENEER_AREA + 16 * nslots, slot = page + SLOT_AREA + 8 * nslots;
    for (unsigned i = 0; i < 16; i++) if (m->base[veneer + i]) { snprintf(why, why_len, "the veneer area is not empty"); return -1; }
    for (unsigned i = 0; i < 8; i++) if (m->base[slot + i]) { snprintf(why, why_len, "the slot area is not empty"); return -1; }
    uint32_t orig, rd, sf; uint64_t v;
    memcpy(&orig, m->base + off, 4);
    if (!mov_imm(orig, &rd, &sf, &v)) { snprintf(why, why_len, "main+0x%llx is not a MOVZ/MOVN", (unsigned long long)off); return -1; }
    uint32_t to = enc_b(off, veneer), back = enc_b(veneer + 8, off + 4);
    if (!to || !back) { snprintf(why, why_len, "main+0x%llx is out of branch range of the veneer", (unsigned long long)off); return -1; }
    uint32_t lo = (uint32_t)(slot & 0xfff);
    uint32_t ldr = sf ? 0xf9400000u | ((lo / 8) << 10) | (rd << 5) | rd : 0xb9400000u | ((lo / 4) << 10) | (rd << 5) | rd;
    uint32_t code[3] = {enc_adrp(rd, veneer, slot), ldr, back};
    memcpy(m->base + veneer, code, sizeof code);
    memcpy(m->base + slot, &v, 8);   /* the original value until a section writes */
    memcpy(m->base + off, &to, 4);
    slots[nslots] = (Slot){off, rd, sf};
    fprintf(stderr, "[cheats] main+0x%llx (%s %c%u) now loads its value from a cheat slot (veneer +0x%llx)\n", (unsigned long long)off,
            "mov", sf ? 'x' : 'w', rd, (unsigned long long)veneer);
    return (int)nslots++;
}

static void live_write(const CheatOp *o) {
    Module *m = live_module;
    if (o->slot >= 0) {
        uint32_t rd, sf; uint64_t v;
        if (mov_imm((uint32_t)o->value, &rd, &sf, &v))
            atomic_store_explicit((_Atomic uint64_t *)(void *)(m->base + private_page(m) + SLOT_AREA + 8 * (unsigned)o->slot), v, memory_order_relaxed);
        return;
    }
    switch (o->width) {
    case 1: atomic_store_explicit((_Atomic uint8_t *)(void *)(m->base + o->off), (uint8_t)o->value, memory_order_relaxed); break;
    case 2: { uint16_t v = (uint16_t)o->value; memcpy(m->base + o->off, &v, 2); break; }
    case 4: { uint32_t v = (uint32_t)o->value; memcpy(m->base + o->off, &v, 4); break; }
    default: memcpy(m->base + o->off, &o->value, 8);
    }
}

static void run_section(LiveSection *s, uint64_t held) {
    int active = 1, stack[LIVE_MAX_DEPTH + 1], depth = 0, fired = 0;
    for (unsigned i = s->first; i < s->first + s->count; i++) {
        const CheatOp *o = &live_ops[i];
        switch (o->op) {
        case OP_KEY: if (depth <= LIVE_MAX_DEPTH) stack[depth++] = active; active = active && (held & o->keys) == o->keys; if (active) fired = 1; break;
        case OP_ELSE: if (depth) active = stack[depth - 1] && !active; break;
        case OP_END: if (depth) active = stack[--depth]; break;
        default: if (active) live_write(o);
        }
    }
    if (fired && !s->active_logged) { s->active_logged = 1; fprintf(stderr, "[cheats] [%s] button held: active\n", s->name); }
}

void runtime_cheats_input(uint64_t buttons, int lx, int ly, int rx, int ry) {
    if (!live_nsections) return;
    const int t = 16384;
    uint64_t held = buttons & 0xffff;
    if (lx < -t) held |= 1u << 16; if (ly > t) held |= 1u << 17; if (lx > t) held |= 1u << 18; if (ly < -t) held |= 1u << 19;
    if (rx < -t) held |= 1u << 20; if (ry > t) held |= 1u << 21; if (rx > t) held |= 1u << 22; if (ry < -t) held |= 1u << 23;
    for (unsigned i = 0; i < live_nsections; i++) run_section(&live_sections[i], held);
}

/* After sealing: read-only pages that button sections write become writable again (they hold data, never code). */
void runtime_cheats_sealed(Module *m) {
#if TARGET_OS_IPHONE
    (void)m;
#else
    if (m != live_module) return;
    for (unsigned i = 0; i < nro_pages; i++)
        if (mprotect(m->base + ro_pages[i], 0x4000, PROT_READ | PROT_WRITE)) fprintf(stderr, "[cheats] cannot make main+0x%llx writable\n", (unsigned long long)ro_pages[i]);
#endif
}

static int add_live(Module *m, const char *name, CheatOp *ops, unsigned n, char *why, size_t why_len) {
    if (live_nsections >= LIVE_MAX_SECTIONS || live_nops + n > LIVE_MAX_OPS) { snprintf(why, why_len, "too many button codes"); return 0; }
    for (unsigned i = 0; i < n; i++) {   /* claim veneers / pages first; a veneer left by a failed section keeps loading the original value */
        CheatOp *o = &ops[i];
        if (o->op != OP_WRITE) continue;
        if (o->off < page_up16k(m->text_end)) { if ((o->slot = (int16_t)slot_for(m, o->off, why, why_len)) < 0) return 0; continue; }
        uint64_t page = o->off & ~0x3fffull;
        if (page < m->data_off && (o->off + o->width + 0x3fff) / 0x4000 - o->off / 0x4000 > 1) { snprintf(why, why_len, "a write crosses a page"); return 0; }
        if (o->off < (m->data_off & ~0x3fffull)) {
            unsigned k = 0; while (k < nro_pages && ro_pages[k] != page) k++;
            if (k == nro_pages) ro_pages[nro_pages++] = page;
        }
    }
    LiveSection *s = &live_sections[live_nsections++];
    snprintf(s->name, sizeof s->name, "%s", name); s->first = live_nops; s->count = n; s->active_logged = 0;
    memcpy(live_ops + live_nops, ops, n * sizeof *ops); live_nops += n;
    return 1;
}

static void apply_section(Module *m, const char *name, const uint32_t *w, unsigned n, unsigned *applied, unsigned *skipped) {
    static CheatOp ops[CHEAT_MAX_WORDS];
    char why[200]; unsigned nops; int kind;
    if (!n) return;
    if (decode(m, w, n, ops, &nops, &kind, why, sizeof why)) { fprintf(stderr, "[cheats] skipped [%s]: %s\n", name, why); ++*skipped; return; }
    if (kind == 2) return;   /* folder marker */
    if (kind == 1) {
        if (!add_live(m, name, ops, nops, why, sizeof why)) { fprintf(stderr, "[cheats] skipped [%s]: %s\n", name, why); ++*skipped; return; }
        unsigned nw = 0; for (unsigned i = 0; i < nops; i++) nw += ops[i].op == OP_WRITE;
        fprintf(stderr, "[cheats] armed [%s]: %u write%s switched by buttons while playing\n", name, nw, nw == 1 ? "" : "s");
        ++*applied; return;
    }
    for (unsigned i = 0; i < nops; i++) {
        const CheatOp *c = &ops[i]; uint64_t old = 0;
        memcpy(&old, m->base + c->off, c->width);
        memcpy(m->base + c->off, &c->value, c->width);
        fprintf(stderr, "[cheats] [%s] main+0x%llx (%s): %u bytes 0x%0*llx -> 0x%0*llx\n", name, (unsigned long long)c->off, region(m, c->off),
                c->width, (int)(2 * c->width), (unsigned long long)old, (int)(2 * c->width), (unsigned long long)c->value);
    }
    fprintf(stderr, "[cheats] applied [%s]: %u write%s\n", name, nops, nops == 1 ? "" : "s");
    ++*applied;
}

void runtime_cheats(Module *m) {
#if TARGET_OS_IPHONE
    (void)m;   // the text is already sealed shared pages on iOS
#else
    const char *off = getenv("TOTK_CHEATS"), *dir = getenv("TOTK_CHEATS_DIR");
    if ((off && !strcmp(off, "0")) || !dir || !*dir) return;
    char id[17];
    if (!main_build_id(m, id)) { fprintf(stderr, "[cheats] main module build ID not found; cheats not loaded\n"); return; }
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s.txt", dir, id) >= (int)sizeof path) return;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[cheats] none (%s not found)\n", path); return; }
    static char text[CHEAT_MAX_FILE + 1];
    size_t len = fread(text, 1, CHEAT_MAX_FILE + 1, f); fclose(f);
    if (len > CHEAT_MAX_FILE) { fprintf(stderr, "[cheats] %s is larger than 1 MB; cheats not loaded\n", path); return; }
    text[len] = 0;
    fprintf(stderr, "[cheats] loading %s (main build ID %s)\n", path, id);
    live_module = m;

    static uint32_t words[CHEAT_MAX_WORDS];
    unsigned nwords = 0, applied = 0, skipped = 0, disabled = 0, have = 0, enabled = 0, bad_line = 0;
    char name[256] = "";
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n'); if (next) *next++ = 0;
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        size_t ll = strlen(line); while (ll && (line[ll - 1] == ' ' || line[ll - 1] == '\t' || line[ll - 1] == '\r')) line[--ll] = 0;
        if (!ll) continue;
        int header = line[0] == '[' || line[0] == '{', off_header = line[0] == '#' && line[1] == '[';
        if (header || off_header) {
            if (have && enabled && !bad_line) apply_section(m, name, words, nwords, &applied, &skipped);
            else if (have && enabled) { fprintf(stderr, "[cheats] skipped [%s]: a line is not 8-digit hex words\n", name); skipped++; }
            const char *s = line + (off_header ? 2 : 1); size_t n = strcspn(s, "]}");
            snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name - 1 ? n : sizeof name - 1), s);
            have = 1; enabled = !off_header; nwords = 0; bad_line = 0;
            if (off_header) disabled++;
            continue;
        }
        if (line[0] == '#' || (line[0] == '/' && line[1] == '/')) continue;
        if (!have) { fprintf(stderr, "[cheats] ignoring codes before the first [section] header\n"); continue; }
        for (char *p = line; *p;) {
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strcspn(p, " \t"); if (!n) break;
            if (!is_hex_word(p, n) || nwords >= CHEAT_MAX_WORDS) { bad_line = 1; break; }
            char tok[9]; memcpy(tok, p, 8); tok[8] = 0;
            words[nwords++] = (uint32_t)strtoul(tok, NULL, 16);
            p += n;
        }
    }
    if (have && enabled && !bad_line) apply_section(m, name, words, nwords, &applied, &skipped);
    else if (have && enabled) { fprintf(stderr, "[cheats] skipped [%s]: a line is not 8-digit hex words\n", name); skipped++; }
    runtime_cheats_input(0, 0, 0, 0, 0);   /* button sections start in their released state */
    fprintf(stderr, "[cheats] %u section%s applied (%u switched by buttons), %u skipped, %u disabled\n", applied, applied == 1 ? "" : "s",
            live_nsections, skipped, disabled);
#endif
}
