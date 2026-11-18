// The corner box's settings (front_hud.m DmCornerHUD), pure so the native UI test drives the same code as the game (front_mac.m).
// The box shows lines the player picks: FPS, the on-demand shader / pipeline counters, frame timing, the frame-time graph and the
// shortcut list. Key 1 shows or hides the whole box and keeps its lines; keys 2-6 add or remove one line (front_shortcuts.h).
// Saved in controller-layout.txt as hud=0|1 and hud_lines=<names, comma-separated, may be empty>. A file without hud= was written
// by an earlier build: what it showed is kept (hints=1 the shortcut list, stats=1 FPS and shaders, stats=2 also timing,
// frame_graph=1 the graph; no hints= key meant shown); no file at all: the box starts hidden.
// DM_AOC_STATS / DM_AOC_FRAME_GRAPH impose lines for one run and are never saved: DM_AOC_STATS 0 hides FPS, shaders and timing,
// 1 shows FPS and shaders, 2 all three, another value counts as 0 (invalid); DM_AOC_FRAME_GRAPH 1 shows the graph, anything
// else hides it. Imposing a line that shows turns the box on for the run. A key or a setting on an imposed line ends the
// imposition of that line and becomes the player's value; lines nobody touched are saved with the player's own value.
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { HUD_FPS = 1, HUD_SHADERS = 2, HUD_TIMING = 4, HUD_GRAPH = 8, HUD_KEYS = 16, HUD_LINES_ALL = 31, HUD_DEFAULT_LINES = HUD_FPS | HUD_SHADERS };
typedef struct { int on, lines; } DmHud;                                  // the player's choice, the one saved
typedef struct { int mask, value, on, invalid; } DmHudImpose;             // the environment's, this run only
static const char *const dm_hud_names[] = {"fps", "shaders", "timing", "graph", "keys"};

static inline DmHud dm_hud_parse(const char *text) {
    DmHud h = {0, HUD_DEFAULT_LINES};
    if (!text) return h;                                                  // no settings file: hidden
    int has_hud = 0, hints = 1, stats = 0, graph = 0, lines = 0;
    char *copy = strdup(text), *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; char *key = line, *value = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        for (char *e = value + strlen(value); e > value && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'); ) *--e = 0;
        while (*value == ' ' || *value == '\t') value++;
        if (!strcmp(key, "hud")) { has_hud = 1; h.on = atoi(value) != 0; }
        else if (!strcmp(key, "hud_lines")) {
            char *s2 = NULL;
            for (char *n = strtok_r(value, ",", &s2); n; n = strtok_r(NULL, ",", &s2))
                for (int i = 0; i < 5; i++) if (!strcmp(n, dm_hud_names[i])) lines |= 1 << i;
        }
        else if (!strcmp(key, "hints")) hints = atoi(value) != 0;
        else if (!strcmp(key, "stats")) { int n = atoi(value); stats = n >= 0 && n <= 2 ? n : 0; }
        else if (!strcmp(key, "frame_graph")) graph = atoi(value) != 0;
    }
    free(copy);
    if (has_hud) { h.lines = lines; return h; }
    h.lines = (hints ? HUD_KEYS : 0) | (stats >= 1 ? HUD_FPS | HUD_SHADERS : 0) | (stats == 2 ? HUD_TIMING : 0) | (graph ? HUD_GRAPH : 0);
    h.on = h.lines != 0;
    if (!h.lines) h.lines = HUD_DEFAULT_LINES;
    return h;
}
static inline void dm_hud_format(DmHud h, char *out, size_t n) {
    int used = snprintf(out, n, "hud=%d\nhud_lines=", !!h.on);
    for (int i = 0, first = 1; i < 5 && used > 0 && (size_t)used < n; i++)
        if (h.lines & 1 << i) { used += snprintf(out + used, n - used, "%s%s", first ? "" : ",", dm_hud_names[i]); first = 0; }
    if (used > 0 && (size_t)used < n) snprintf(out + used, n - used, "\n");
}
static inline DmHudImpose dm_hud_impose(const char *stats, const char *graph) {
    DmHudImpose m = {0, 0, 0, 0};
    if (stats) {
        int v = (stats[0] >= '0' && stats[0] <= '2' && !stats[1]) ? stats[0] - '0' : 0;
        m.invalid = !(stats[0] >= '0' && stats[0] <= '2' && !stats[1]);
        m.mask |= HUD_FPS | HUD_SHADERS | HUD_TIMING;
        m.value |= (v >= 1 ? HUD_FPS | HUD_SHADERS : 0) | (v == 2 ? HUD_TIMING : 0);
        if (v >= 1) m.on = 1;
    }
    if (graph) { m.mask |= HUD_GRAPH; if (!strcmp(graph, "1")) { m.value |= HUD_GRAPH; m.on = 1; } }
    return m;
}
static inline DmHud dm_hud_effective(DmHud h, DmHudImpose m) {
    return (DmHud){m.on ? 1 : h.on, (h.lines & ~m.mask) | (m.value & m.mask)};
}
static inline int dm_hud_visible(DmHud eff) { return eff.on && eff.lines; }
// Statistics are collected while a line that needs them shows (aoc_stats_enable): otherwise the box would show frozen numbers.
static inline int dm_hud_needs_stats(DmHud eff) { return dm_hud_visible(eff) && (eff.lines & (HUD_FPS | HUD_SHADERS | HUD_TIMING | HUD_GRAPH)); }
static inline void dm_hud_set_line(DmHud *h, DmHudImpose *m, int bit, int on) {
    DmHud eff = dm_hud_effective(*h, *m);
    h->lines = (eff.lines & bit ? h->lines | bit : h->lines & ~bit);   // the imposed value becomes the player's, then the change
    h->lines = on ? h->lines | bit : h->lines & ~bit;
    m->mask &= ~bit;
}
static inline void dm_hud_set_on(DmHud *h, DmHudImpose *m, int on) {
    h->on = on != 0; m->on = 0;
    if (on && !dm_hud_effective(*h, *m).lines) { h->lines = HUD_DEFAULT_LINES; m->mask &= ~HUD_DEFAULT_LINES; }   // never an empty box
}
// A key: 0 shows or hides the box (its lines kept); a line bit adds or removes that line, or, with the box hidden, shows the box with it.
static inline void dm_hud_key(DmHud *h, DmHudImpose *m, int bit) {
    DmHud eff = dm_hud_effective(*h, *m);
    if (!bit) { dm_hud_set_on(h, m, !dm_hud_visible(eff)); return; }
    if (!dm_hud_visible(eff)) { dm_hud_set_line(h, m, bit, 1); h->on = 1; m->on = 0; return; }
    dm_hud_set_line(h, m, bit, !(eff.lines & bit));
}
