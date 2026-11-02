// Super Smash Bros. Ultimate (Q96): the DLC in a folder of the player's own NSPs, read without keys.
// An NSP is a PFS0 container whose header is not encrypted. A DLC's holds <rights id>.tik, whose first 16 hex digits are
// the DLC's title id, and a .cnmt.nca; the index the game asks for (nn::aoc::ListAddOnContent) is the title id minus the
// application id + 0x1000. Smash's DLC carry only a marker file (the fighters and costumes ship in the update), so the
// index is all the game needs. Shared by the Mac (the DLC menu's folder) and iOS (Documents/DLC, Q75).
#pragma once
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define AOC_FIGHTERS 12   // indices 1-12 are the fighters: Piranha Plant, Challenger Packs 1-11 (99 NSPs read, 25.10.2026)
static int aoc_fighters(const int *index, int n) { int f = 0; for (int i = 0; i < n; i++) if (index[i] >= 1 && index[i] <= AOC_FIGHTERS) f++; return f; }

/* The title id in a .cnmt.xml (plain text, beside the meta NCA in NSPs without a ticket): the content meta's own <Id>, if
   its <Type> is AddOnContent and its <ApplicationId> is app_id; 0 otherwise. */
static uint64_t aoc_cnmt_xml_title(const char *x, uint64_t app_id) {
    const char *type = strstr(x, "<Type>"), *id = strstr(x, "<Id>"), *app = strstr(x, "<ApplicationId>");
    if (!type || !id || !app || strncmp(type, "<Type>AddOnContent</Type>", 25)) return 0;
    char *end; uint64_t a = strtoull(app + 15, &end, 16), t = strtoull(id + 4, &end, 16);
    return a == app_id && *end == '<' ? t : 0;
}

/* The DLC index of one NSP, or -1: not readable, not a PFS0, no meta NCA, no title (neither a ticket, whose name starts
   with the title id, nor a .cnmt.xml), or a title that is not a DLC of app_id (the base game, an update, another game) or
   two different titles. */
static int aoc_nsp_index(const char *path, uint64_t app_id, int max) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t h[16]; uint32_t n = 0, st = 0;
    if (fread(h, 1, 16, f) != 16 || memcmp(h, "PFS0", 4)) { fclose(f); return -1; }
    memcpy(&n, h + 4, 4); memcpy(&st, h + 8, 4);
    if (!n || n > 64 || !st || st > 4096) { fclose(f); return -1; }
    size_t len = (size_t)n * 24 + st;
    uint8_t *b = (uint8_t *)malloc(len);
    if (!b || fread(b, 1, len, f) != len) { free(b); fclose(f); return -1; }
    const char *names = (const char *)b + (size_t)n * 24;
    uint64_t base = app_id + 0x1000, title = 0;
    int meta = 0, bad = 0;
    for (uint32_t i = 0; i < n && !bad; i++) {
        uint32_t at; uint64_t off, size;
        memcpy(&off, b + (size_t)i * 24, 8); memcpy(&size, b + (size_t)i * 24 + 8, 8); memcpy(&at, b + (size_t)i * 24 + 16, 4);
        if (at >= st) { bad = 1; break; }
        size_t l = strnlen(names + at, st - at);
        if (l == st - at) { bad = 1; break; }
        const char *name = names + at;
        uint64_t t = 0;
        if (l > 9 && !strcasecmp(name + l - 9, ".cnmt.nca")) meta = 1;
        if (l == 36 && !strcasecmp(name + 32, ".tik")) {
            char hex[17]; memcpy(hex, name, 16); hex[16] = 0;
            char *end; t = strtoull(hex, &end, 16);
            if (*end || !t) bad = 1;
        } else if (l > 9 && !strcasecmp(name + l - 9, ".cnmt.xml") && size > 0 && size <= 65536) {
            char *x = (char *)malloc(size + 1);
            if (x && !fseeko(f, (off_t)(16 + len + off), SEEK_SET) && fread(x, 1, size, f) == size) { x[size] = 0; t = aoc_cnmt_xml_title(x, app_id); }
            free(x);
            if (!t) bad = 1;
        }
        if (t && !bad) {
            if (t <= base || t - base >= (uint64_t)max || (title && title != t)) bad = 1;
            else title = t;
        }
    }
    fclose(f); free(b);
    return !bad && meta && title ? (int)(title - base) : -1;
}

/* The DLC indices of the .nsp files in dir, sorted and each once, at most max - 1 of them; *nsps counts the .nsp files
   seen, *skipped those that are not a DLC of app_id. Returns -1 when dir cannot be opened. */
static int aoc_scan_dir(const char *dir, uint64_t app_id, int *out, int max, int *nsps, int *skipped) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0; *nsps = *skipped = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || e->d_name[0] == '.' || strcasecmp(e->d_name + l - 4, ".nsp")) continue;
        (*nsps)++;
        char path[4096];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path) { (*skipped)++; continue; }
        int index = aoc_nsp_index(path, app_id, max);
        if (index < 0) { (*skipped)++; continue; }
        int seen = 0; for (int i = 0; i < n; i++) if (out[i] == index) seen = 1;
        if (!seen && n < max - 1) out[n++] = index;
    }
    closedir(d);
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && out[j - 1] > out[j]; j--) { int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t; }
    return n;
}
