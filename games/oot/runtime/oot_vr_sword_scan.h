/* Diagnostic, read only, TOTK_VR_SWORDSCAN=1: where the physics objects of the sword keep its place. From the actor's physics component
   ([actor+0x270]+0x50) it follows pointers and reports every place holding the game's own position of the sword (G) or the one the
   player's hand gave it (F), as three floats in a row or as the translation column of a 3x4. A few scans a session, on the game
   thread. Reads go through vm_read_overwrite, so a stale pointer ends that branch. Included by oot_vr_sword.h. */
#pragma once
#include <mach/mach.h>

enum { OOT_SCAN_NODES = 4000, OOT_SCAN_SEEN = 16384, OOT_SCAN_SPAN = 0x800, OOT_SCAN_DEPTH = 5, OOT_SCAN_LINES = 90, OOT_SCAN_RUNS = 8, OOT_SCAN_EVERY = 40 };
static const float OOT_SCAN_NEAR = 0.02f;
typedef struct { uintptr_t at; int parent; unsigned from, depth; } oot_scan_node;

static int oot_scan_read(uintptr_t at, void *out, size_t size) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)at, size, (vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}
static int oot_scan_mark(uintptr_t *seen, uintptr_t at) {
    for (unsigned i = (unsigned)((at >> 4) * 2654435761u) % OOT_SCAN_SEEN, k = 0; k < OOT_SCAN_SEEN; k++, i = (i + 1) % OOT_SCAN_SEEN) {
        if (seen[i] == at) return 0;
        if (!seen[i]) { seen[i] = at; return 1; }
    }
    return 0;
}
static int oot_scan_near(const float *p, unsigned stride, const float m[12]) {
    for (unsigned i = 0; i < 3; i++) if (!(fabsf(p[i * stride] - m[4 * i + 3]) < OOT_SCAN_NEAR)) return 0;
    return 1;
}
static void oot_scan_say(const oot_scan_node *nodes, int node, unsigned off, const char *which, const char *layout, int run) {
    unsigned path[OOT_SCAN_DEPTH + 1], n = 0;
    for (int k = node; k > 0 && n <= OOT_SCAN_DEPTH; k = nodes[k].parent) path[n++] = nodes[k].from;
    char text[96]; int o = 0;
    while (n && o < 80) o += snprintf(text + o, sizeof text - o, "+%#x>", path[--n]);
    text[o] = 0;
    fprintf(stderr, "[vr-scan] run %d %s %s at %#llx physics %s+%#x\n", run, which, layout, (unsigned long long)(nodes[node].at + off), text, off);
}
static void oot_scan_block(oot_scan_node *nodes, int *count, uintptr_t *seen, int node, const float game[12], const float final[12], int run, int *lines) {
    static uint32_t raw[OOT_SCAN_SPAN / 4];
    if (!oot_scan_read(nodes[node].at, raw, sizeof raw)) return;
    const float *f = (const float *)(const void *)raw;
    for (unsigned i = 0; i + 9 < OOT_SCAN_SPAN / 4 && *lines < OOT_SCAN_LINES; i++) {
        for (unsigned s = 0; s < 2; s++) {
            const unsigned stride = s ? 4 : 1;
            const char *layout = s ? "rows" : "xyz";
            if (oot_scan_near(f + i, stride, game)) { oot_scan_say(nodes, node, i * 4, "G", layout, run); ++*lines; }
            if (oot_scan_near(f + i, stride, final)) { oot_scan_say(nodes, node, i * 4, "F", layout, run); ++*lines; }
        }
    }
    if (nodes[node].depth >= OOT_SCAN_DEPTH) return;
    for (unsigned i = 0; i + 1 < OOT_SCAN_SPAN / 4 && *count < OOT_SCAN_NODES; i += 2) {
        uint64_t p; memcpy(&p, raw + i, 8);
        if ((p & 7) || p < 0x100000000ull || p >= 0x8000000000ull || !oot_scan_mark(seen, (uintptr_t)p)) continue;
        nodes[*count] = (oot_scan_node){ (uintptr_t)p, node, i * 4, nodes[node].depth + 1 };
        ++*count;
    }
}
static _Atomic uint64_t oot_vr_cuts_written;
/* Each swing queues cuts for the field grass with the points its blade passed: where and when the game's own attack reckons the
   sword is. The module is at main+0x4e22540, its scene at [module+0xf0]+0x338, the queue at scene+0x20 (capacity, entries of 16
   bytes at +0x28, write index +0x38); a slot holds its place at +0, radius at +0xc, three points at +0x18, +0x24, +0x30. */
static void oot_vr_cut_log(const float game[12], const float final[12]) {
    static int on = -1, lines; static uint64_t last = ~0ull;
    if (on < 0) { const char *e = getenv("TOTK_VR_SWORDSCAN"); on = e && *e == '1'; }
    if (!on || lines >= 400) return;
    uint64_t module = 0, holder = 0, scene = 0, entries = 0, written = 0; uint32_t capacity = 0; uint8_t off = 1;
    if (!oot_scan_read((uintptr_t)oot_vr_base + 0x4e22540, &module, 8) || !module || !oot_scan_read(module + 0x30, &off, 1) || (off & 1)) return;
    if (!oot_scan_read(module + 0xf0, &holder, 8) || !holder || !oot_scan_read(holder + 0x338, &scene, 8) || !scene) return;
    if (!oot_scan_read(scene + 0x20, &capacity, 4) || !oot_scan_read(scene + 0x28, &entries, 8) || !oot_scan_read(scene + 0x38, &written, 8) || !capacity || capacity > 256 || !entries) return;
    atomic_store(&oot_vr_cuts_written, written);
    if (last == ~0ull || written < last || written - last > capacity) last = written;
    for (; last < written && lines < 400; last++, lines++) {
        uint64_t slot = 0; float s[15];
        if (!oot_scan_read(entries + 16 * (last % capacity), &slot, 8) || !slot || !oot_scan_read(slot, s, sizeof s)) continue;
        fprintf(stderr, "[vr-cut] %.3f at %.3f %.3f %.3f radius %.2f points %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f; G %.3f %.3f %.3f F %.3f %.3f %.3f\n",
                (double)oot_vr_now() / 1e9, (double)s[0], (double)s[1], (double)s[2], (double)s[3], (double)s[6], (double)s[7], (double)s[8], (double)s[9], (double)s[10], (double)s[11],
                (double)s[12], (double)s[13], (double)s[14], (double)game[3], (double)game[7], (double)game[11], (double)final[3], (double)final[7], (double)final[11]);
    }
}
/* The words of the sword's physics objects that differ only while the attack window is open: a snapshot kept fresh while nothing is
   happening, compared when a cut appears, and kept if the word is back to its old value half a second later. */
enum { OOT_WIN_NODES = 1200, OOT_WIN_WORDS = 0x100, OOT_WIN_CANDS = 400, OOT_WIN_ROUNDS = 4, OOT_WIN_REFRESH = 16 };
static const uint64_t OOT_WIN_SETTLE_NS = 450000000ull;
static struct {
    oot_scan_node nodes[OOT_WIN_NODES];
    uint32_t rest[OOT_WIN_NODES][OOT_WIN_WORDS];
    uintptr_t seen[OOT_SCAN_SEEN];
    struct { int node; unsigned word; uint32_t rest, during; } cand[OOT_WIN_CANDS];
    int count, cands, stage, rounds;
    unsigned tick;
    uint64_t cuts, at;
} oot_win;

static void oot_win_snapshot(uintptr_t physics) {
    memset(oot_win.seen, 0, sizeof oot_win.seen);
    oot_win.nodes[0] = (oot_scan_node){ physics, -1, 0, 0 };
    oot_scan_mark(oot_win.seen, physics);
    oot_win.count = 1;
    for (int k = 0; k < oot_win.count; k++) {
        uint32_t *block = oot_win.rest[k];
        if (!oot_scan_read(oot_win.nodes[k].at, block, sizeof oot_win.rest[k])) { memset(block, 0, sizeof oot_win.rest[k]); continue; }
        if (oot_win.nodes[k].depth >= 4) continue;
        for (unsigned i = 0; i + 1 < OOT_WIN_WORDS && oot_win.count < OOT_WIN_NODES; i += 2) {
            uint64_t p; memcpy(&p, block + i, 8);
            if ((p & 7) || p < 0x100000000ull || p >= 0x8000000000ull || !oot_scan_mark(oot_win.seen, (uintptr_t)p)) continue;
            oot_win.nodes[oot_win.count++] = (oot_scan_node){ (uintptr_t)p, k, i * 4, oot_win.nodes[k].depth + 1 };
        }
    }
}
static void oot_win_compare(void) {
    static uint32_t now[OOT_WIN_WORDS];
    oot_win.cands = 0;
    for (int k = 0; k < oot_win.count && oot_win.cands < OOT_WIN_CANDS; k++) {
        if (!oot_scan_read(oot_win.nodes[k].at, now, sizeof now)) continue;
        for (unsigned i = 0; i < OOT_WIN_WORDS && oot_win.cands < OOT_WIN_CANDS; i++)
            if (now[i] != oot_win.rest[k][i]) { oot_win.cand[oot_win.cands].node = k; oot_win.cand[oot_win.cands].word = i; oot_win.cand[oot_win.cands].rest = oot_win.rest[k][i]; oot_win.cand[oot_win.cands++].during = now[i]; }
    }
}
static void oot_win_report(void) {
    int kept = 0;
    for (int c = 0; c < oot_win.cands; c++) {
        uint32_t v;
        if (!oot_scan_read(oot_win.nodes[oot_win.cand[c].node].at + oot_win.cand[c].word * 4, &v, 4) || v != oot_win.cand[c].rest) continue;
        char path[96]; int o = 0; unsigned steps[OOT_SCAN_DEPTH + 1], n = 0;
        for (int k = oot_win.cand[c].node; k > 0 && n <= OOT_SCAN_DEPTH; k = oot_win.nodes[k].parent) steps[n++] = oot_win.nodes[k].from;
        while (n && o < 80) o += snprintf(path + o, sizeof path - o, "+%#x>", steps[--n]);
        path[o] = 0;
        fprintf(stderr, "[vr-window] round %d at %#llx physics %s+%#x rest %08x during %08x\n", oot_win.rounds, (unsigned long long)(oot_win.nodes[oot_win.cand[c].node].at + oot_win.cand[c].word * 4),
                path, oot_win.cand[c].word * 4, oot_win.cand[c].rest, oot_win.cand[c].during);
        kept++;
    }
    fprintf(stderr, "[vr-window] round %d: %d objects, %d words changed, %d of them only in the window\n", oot_win.rounds, oot_win.count, oot_win.cands, kept);
}
static void oot_vr_window_diff(uintptr_t actor, uint64_t cuts) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_VR_SWORDSCAN"); on = e && *e == '1'; }
    if (!on || oot_win.rounds >= OOT_WIN_ROUNDS) return;
    const uintptr_t components = oot_vr_load64(actor + 0x270), physics = components ? oot_vr_load64(components + 0x50) : 0;
    if (!physics) return;
    const uint64_t now = oot_vr_now();
    if (oot_win.stage == 1) {
        if (now - oot_win.at >= OOT_WIN_SETTLE_NS) { oot_win_report(); oot_win.rounds++; oot_win.stage = 0; oot_win.tick = 0; oot_win.count = 0; }
        return;
    }
    if (oot_win.count && cuts != oot_win.cuts) { oot_win_compare(); oot_win.stage = 1; oot_win.at = now; oot_win.cuts = cuts; return; }
    if (!oot_win.count || ++oot_win.tick >= OOT_WIN_REFRESH) { oot_win_snapshot(physics); oot_win.cuts = cuts; oot_win.tick = 0; }
}
/* From oot_vr_place, once the weapon has followed the hand for a while. */
static void oot_vr_sword_scan(uintptr_t actor, const float game[12], const float final[12]) {
    static int on = -1, runs; static unsigned updates; static _Atomic int busy;
    static oot_scan_node nodes[OOT_SCAN_NODES]; static uintptr_t seen[OOT_SCAN_SEEN];
    if (on < 0) { const char *e = getenv("TOTK_VR_SWORDSCAN"); on = e && *e == '1'; }
    if (!on || runs >= OOT_SCAN_RUNS || ++updates % OOT_SCAN_EVERY || atomic_exchange(&busy, 1)) return;
    const uintptr_t components = oot_vr_load64(actor + 0x270), physics = components ? oot_vr_load64(components + 0x50) : 0;
    if (physics) {
        memset(seen, 0, sizeof seen);
        nodes[0] = (oot_scan_node){ physics, -1, 0, 0 };
        oot_scan_mark(seen, physics);
        int count = 1, lines = 0;
        const uint64_t t0 = oot_vr_now();
        for (int k = 0; k < count && lines < OOT_SCAN_LINES; k++) oot_scan_block(nodes, &count, seen, k, game, final, runs, &lines);
        fprintf(stderr, "[vr-scan] run %d: %d objects, %d places, %.1f ms; G %.3f %.3f %.3f F %.3f %.3f %.3f\n", runs, count, lines, (double)(oot_vr_now() - t0) / 1e6,
                (double)game[3], (double)game[7], (double)game[11], (double)final[3], (double)final[7], (double)final[11]);
        runs++;
    }
    atomic_store(&busy, 0);
}
