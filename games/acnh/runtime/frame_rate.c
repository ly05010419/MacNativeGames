/* Optional ACNH 60 FPS (3.0.1, 3.0.3) and 120 FPS (3.0.3). The renderer supplies the cadence; a locally
 * prepared, licensed SpeedFix supplies the per-update speed corrections of each rate.
 * Selection applies at the next launch: code and parameters must change together.
 * Original game files and rewritten binaries are never modified. */
#include "runtime.h"
#include "game_version.h"
#include "fps_config.h"
#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef ACNH_FPS120_ENABLED
#define ACNH_FPS120_ENABLED 0
#endif
#define FPS_BUILD (ACNH_FPS_ENABLED && ACNH_SDK_3_0)
#define FPS120_BUILD (FPS_BUILD && ACNH_FPS120_ENABLED)

/* One row per prepared rate: where the launcher says its assets are, the hashes the build pinned, and the identity the
 * packager looks for in this binary. */
typedef struct {
    int fps; const char *env, *manifest_sha, *patch_sha, *romfs_sha, *identity; unsigned count;
    char root[PATH_MAX]; int available;
} Rate;
static Rate rates[] = {
#if FPS_BUILD
    {60, "ACNH_FPS_ASSETS", ACNH_FPS_MANIFEST_SHA, ACNH_FPS_PATCH_SHA, ACNH_FPS_ROMFS_SHA,
     "MNG_ACNH_FPS_ASSETS:" ACNH_FPS_MANIFEST_SHA, ACNH_FPS_PATCH_COUNT, {0}, 0},
#endif
#if FPS120_BUILD
    {120, "ACNH_FPS120_ASSETS", ACNH_FPS120_MANIFEST_SHA, ACNH_FPS120_PATCH_SHA, ACNH_FPS120_ROMFS_SHA,
     "MNG_ACNH_FPS120_ASSETS:" ACNH_FPS120_MANIFEST_SHA, ACNH_FPS120_PATCH_COUNT, {0}, 0},
#endif
    {0, NULL, NULL, NULL, NULL, NULL, 0, {0}, 0},
};
/* 120 FPS needs a screen that refreshes 120 times a second: the host presents one picture per refresh, so on a 60 Hz
 * screen the game gets 60 updates a second while every speed is corrected for 120, and plays in slow motion (measured
 * on 3.0.3, 2026-11-17: 476 frames in 8.0 s on a 60 Hz MacBook Air panel). An unknown rate (0) does not restrict it. */
static int display_holds(int fps) {
    if (fps < 120) return 1;
    int hz = runtime_display_max_hz();
    return hz <= 0 || hz >= fps;
}
static Rate *rate_of(int fps) {
    for (Rate *r = rates; r->fps; r++) if (r->fps == fps) return r;
    return NULL;
}

static pthread_once_t fps_once = PTHREAD_ONCE_INIT;
static _Atomic int fps_choice = 30;
static int fps_active = 30, fps_available;

static char *checked_asset(const Rate *rate, const char *name, const char *expected) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", rate->root, name) >= (int)sizeof path) runtime_fail("FPS asset path");
    FILE *f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END)) runtime_fail("FPS asset missing");
    long size = ftell(f);
    if (size <= 0 || size > 16 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) runtime_fail("FPS asset size");
    char *data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) runtime_fail("FPS asset read");
    fclose(f); data[size] = 0;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]; char hex[65];
    CC_SHA256(data, (CC_LONG)size, hash);
    for (unsigned i = 0; i < sizeof hash; i++) snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    if (strcmp(hex, expected)) runtime_fail("FPS asset integrity mismatch");
    return data;
}

/* 30 always; 60 and 120 only where this build carries that rate's row. */
static int parse_rate(const char *word) {
    char *end; long value = strtol(word, &end, 10);
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    if (end == word || *end) return 0;
    return value == 30 || value == 60 || (value == 120 && FPS120_BUILD) ? (int)value : 0;
}

static void fps_init(void) {
    for (Rate *r = rates; r->fps; r++) {
        const char *root = getenv(r->env);
        if (!root || !*root) continue;
        char path[PATH_MAX]; struct stat st;
        if (root[0] != '/' || snprintf(r->root, sizeof r->root, "%s", root) >= (int)sizeof r->root)
            runtime_fail("FPS asset folders must be absolute directories");
        if (snprintf(path, sizeof path, "%s/manifest.json", root) >= (int)sizeof path) runtime_fail("FPS manifest path");
        r->available = !stat(path, &st) && S_ISREG(st.st_mode);
        fps_available |= r->available;
    }
    const char *env = getenv("ACNH_FPS"); char word[32]; int requested = 30;
    if (env) {
        requested = parse_rate(env);
        if (!requested) runtime_fail(FPS120_BUILD ? "ACNH_FPS must be 30, 60 or 120" : "ACNH_FPS must be 30 or 60");
    } else if (runtime_setting_word("frame-rate.txt", word, sizeof word)) {
        int saved = parse_rate(word); if (saved) requested = saved;
    }
    Rate *rate = rate_of(requested);
    /* A saved 120 on a screen that cannot show it steps down to 60 (ACNH_FPS=120 still forces it, for diagnostics). */
    if (!env && requested == 120 && rate && rate->available && !display_holds(120)) {
        fprintf(stderr, "[acnh-fps] 120 FPS needs a 120 Hz screen (fastest connected: %d Hz); using 60 FPS\n", runtime_display_max_hz());
        requested = 60; rate = rate_of(60);
    }
    if (requested != 30 && !(rate && rate->available)) {
        fprintf(stderr, "[acnh-fps] %d FPS data unavailable; keeping the original 30 FPS mode\n", requested);
        requested = 30; rate = NULL;
    }
    fps_active = requested; atomic_store(&fps_choice, requested);
    if (rate) {
        free(checked_asset(rate, "manifest.json", rate->manifest_sha));
        free(checked_asset(rate, "romfs/manifest.tsv", rate->romfs_sha));
        char overlay[PATH_MAX];
        if (snprintf(overlay, sizeof overlay, "%s/romfs", rate->root) >= (int)sizeof overlay) runtime_fail("FPS overlay path");
        if (setenv("TOTK_ROMFS_PATCHES", overlay, 1)) runtime_fail("FPS overlay configuration");
        /* 120: the mod's enabler (in main) asks for present interval 1 on every frame, 2 inside the NES / SNES
         * furniture games; the host's present clock has to tick 120 times a second for that to mean 120 and 60. */
        if (fps_active == 120 && setenv("DM_PRESENT_HZ", "120", 1)) runtime_fail("120 FPS present clock");
        fprintf(stderr, "[acnh-fps] %s\n", rate->identity);
    }
    fprintf(stderr, "[acnh-fps] active %d FPS%s\n", fps_active, rate ? " with checked speed corrections (experimental)" : " (original game timing)");
}

int runtime_frame_rate_active(void) { pthread_once(&fps_once, fps_init); return fps_active; }
int runtime_frame_rate_available(void) { runtime_frame_rate_active(); return fps_available; }
int runtime_frame_rate_offers(int fps) { runtime_frame_rate_active(); Rate *r = rate_of(fps); return fps == 30 || (r && r->available && display_holds(fps)); }
int runtime_frame_rate_choice(void) { runtime_frame_rate_active(); return atomic_load(&fps_choice); }
int runtime_frame_rate_locked(void) { return getenv("ACNH_FPS") != NULL; }
int runtime_frame_rate_choose(int value) {
    runtime_frame_rate_active();
    if (runtime_frame_rate_locked() || !runtime_frame_rate_offers(value)) return 0;
    atomic_store(&fps_choice, value);
    char word[8]; snprintf(word, sizeof word, "%d", value);
    runtime_setting_save("frame-rate.txt", word, "frame rate");
    fprintf(stderr, "[acnh-fps] selected %d FPS; active %d FPS until the next launch\n", value, fps_active);
    return 1;
}

/* Optional policy callback used by the generic NVN present code. VI remains at
 * 60 Hz in every mode; doubling VI notifications repeats incomplete pictures. */
unsigned runtime_present_interval(unsigned interval) {
    int active = runtime_frame_rate_active();
    if (active == 60) return 1;
    /* 120: the present clock ticks at 120 Hz and the patched game chooses 1 (120 FPS) or 2 (its 60 FPS console games). */
    if (active == 120) return interval ? interval : 1;
    /* The selectable 30 FPS lock also covers a scene asking to present every
     * refresh. Builds without the selector keep the game's own interval. */
    if (fps_available && interval < 2) return 2;
    return interval;
}

void runtime_frame_rate_install(Module *m) {
    Rate *rate = rate_of(runtime_frame_rate_active());
    if (!rate || strcmp(m->name, "main")) return;
    char *data = checked_asset(rate, "patches.tsv", rate->patch_sha), *save;
#if TARGET_OS_IPHONE
    unsigned long long text_limit = (m->text_end + 0x3fffULL) & ~0x3fffULL;   /* the embedded r-x text (gen_embed.py x0) */
#endif
    char *line = strtok_r(data, "\n", &save);
    if (!line || strcmp(line, "MNG_ACNH_FPS1")) runtime_fail("FPS patch header");
    struct Word { unsigned long long offset; unsigned before, after; };
    struct Word *words = calloc(rate->count, sizeof *words);
    if (!words) runtime_fail("FPS patch allocation");
    unsigned count = 0;
    while ((line = strtok_r(NULL, "\n", &save))) {
        char extra; struct Word w;
        if (count >= rate->count || sscanf(line, "%llx %x %x %c", &w.offset, &w.before, &w.after, &extra) != 3
            || (w.offset & 3) || m->file_size < 4 || w.offset > m->file_size - 4 || w.offset >= m->key_off
            || (count && w.offset <= words[count - 1].offset)) runtime_fail("Invalid FPS patch range/order");
        unsigned actual; memcpy(&actual, m->base + w.offset, 4);
#if TARGET_OS_IPHONE
        /* iOS: the text came with the corrections already applied (loader.c maps main's 60 FPS text, exported on the
           Mac); its words are checked as patched and left alone, only the data words are written below. */
        if (w.offset < text_limit) {
            if (actual != w.after) { fprintf(stderr, "[acnh-fps] main+%#llx expected the patched %08x, found %08x\n", w.offset, w.after, actual);
                                     runtime_fail("FPS text was not exported with this rate's corrections"); }
            words[count++] = w; continue;
        }
#endif
        if (actual != w.before) {
            fprintf(stderr, "[acnh-fps] main+%#llx expected %08x, found %08x\n", w.offset, w.before, actual);
            runtime_fail("FPS patch original instruction mismatch");
        }
        words[count++] = w;
    }
    if (count != rate->count) runtime_fail("FPS patch count mismatch");
    /* Validate every site before changing the first. The loader seals/flushes
     * instruction-cache pages afterwards, before any guest thread executes. */
    for (unsigned i = 0; i < count; i++)
#if TARGET_OS_IPHONE
        if (words[i].offset >= text_limit)
#endif
        memcpy(m->base + words[i].offset, &words[i].after, 4);
    free(words); free(data);
    fprintf(stderr, "[acnh-fps] verified and installed %u main-module speed corrections for %d FPS\n", count, rate->fps);
}
