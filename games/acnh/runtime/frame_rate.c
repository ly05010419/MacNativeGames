/* Optional ACNH 3.0.1 60 FPS. The renderer supplies the cadence; a locally
 * prepared, licensed SpeedFix supplies the per-update speed corrections.
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

static pthread_once_t fps_once = PTHREAD_ONCE_INIT;
static _Atomic int fps_choice = 30;
static int fps_active = 30, fps_available;
#if ACNH_FPS_ENABLED && ACNH_VERSION_RAW == 2097152
static char fps_root[PATH_MAX];
static const char fps_identity[] = "MNG_ACNH_FPS_ASSETS:" ACNH_FPS_MANIFEST_SHA;

static char *checked_asset(const char *name, const char *expected) {
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/%s", fps_root, name) >= (int)sizeof path) runtime_fail("FPS asset path");
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
#endif

static int parse_rate(const char *word) {
    char *end; long value = strtol(word, &end, 10);
    while (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t') end++;
    return end != word && !*end && (value == 30 || value == 60) ? (int)value : 0;
}

static void fps_init(void) {
#if ACNH_FPS_ENABLED && ACNH_VERSION_RAW == 2097152
    const char *root = getenv("ACNH_FPS_ASSETS");
    if (root && *root) {
        char path[PATH_MAX]; struct stat st;
        if (root[0] != '/' || snprintf(fps_root, sizeof fps_root, "%s", root) >= (int)sizeof fps_root)
            runtime_fail("ACNH_FPS_ASSETS must be an absolute directory");
        if (snprintf(path, sizeof path, "%s/manifest.json", root) >= (int)sizeof path) runtime_fail("FPS manifest path");
        fps_available = !stat(path, &st) && S_ISREG(st.st_mode);
    }
#endif
    const char *env = getenv("ACNH_FPS"); char word[32]; int requested = 30;
    if (env) {
        requested = parse_rate(env);
        if (!requested) runtime_fail("ACNH_FPS must be 30 or 60");
    } else if (runtime_setting_word("frame-rate.txt", word, sizeof word)) {
        int saved = parse_rate(word); if (saved) requested = saved;
    }
    if (requested == 60 && !fps_available) {
        fprintf(stderr, "[acnh-fps] 60 FPS data unavailable; keeping the original 30 FPS mode\n");
        requested = 30;
    }
    fps_active = requested; atomic_store(&fps_choice, requested);
#if ACNH_FPS_ENABLED && ACNH_VERSION_RAW == 2097152
    if (fps_active == 60) {
        free(checked_asset("manifest.json", ACNH_FPS_MANIFEST_SHA));
        free(checked_asset("romfs/manifest.tsv", ACNH_FPS_ROMFS_SHA));
        char overlay[PATH_MAX];
        if (snprintf(overlay, sizeof overlay, "%s/romfs", fps_root) >= (int)sizeof overlay) runtime_fail("FPS overlay path");
        if (setenv("TOTK_ROMFS_PATCHES", overlay, 1)) runtime_fail("FPS overlay configuration");
        fprintf(stderr, "[acnh-fps] %s\n", fps_identity);
    }
#endif
    fprintf(stderr, "[acnh-fps] active %d FPS%s\n", fps_active, fps_active == 60 ? " with checked speed corrections (experimental)" : " (original game timing)");
}

int runtime_frame_rate_active(void) { pthread_once(&fps_once, fps_init); return fps_active; }
int runtime_frame_rate_available(void) { runtime_frame_rate_active(); return fps_available; }
int runtime_frame_rate_choice(void) { runtime_frame_rate_active(); return atomic_load(&fps_choice); }
int runtime_frame_rate_locked(void) { return getenv("ACNH_FPS") != NULL; }
int runtime_frame_rate_choose(int value) {
    runtime_frame_rate_active();
    if ((value != 30 && value != 60) || runtime_frame_rate_locked() || (value == 60 && !fps_available)) return 0;
    atomic_store(&fps_choice, value);
    runtime_setting_save("frame-rate.txt", value == 60 ? "60" : "30", "frame rate");
    fprintf(stderr, "[acnh-fps] selected %d FPS; active %d FPS until the next launch\n", value, fps_active);
    return 1;
}

/* Optional policy callback used by the generic NVN present code. VI remains at
 * 60 Hz in both modes; doubling VI notifications repeats incomplete pictures. */
unsigned runtime_present_interval(unsigned interval) {
    if (runtime_frame_rate_active() == 60) return 1;
    /* The selectable 30 FPS lock also covers a scene asking to present every
     * refresh. Builds without the selector keep the game's own interval. */
    if (fps_available && interval < 2) return 2;
    return interval;
}

void runtime_frame_rate_install(Module *m) {
    if (runtime_frame_rate_active() != 60 || strcmp(m->name, "main")) return;
#if ACNH_FPS_ENABLED && ACNH_VERSION_RAW == 2097152
    char *data = checked_asset("patches.tsv", ACNH_FPS_PATCH_SHA), *save;
    char *line = strtok_r(data, "\n", &save);
    if (!line || strcmp(line, "MNG_ACNH_FPS1")) runtime_fail("FPS patch header");
    struct Word { unsigned long long offset; unsigned before, after; };
    struct Word *words = calloc(ACNH_FPS_PATCH_COUNT, sizeof *words);
    if (!words) runtime_fail("FPS patch allocation");
    unsigned count = 0;
    while ((line = strtok_r(NULL, "\n", &save))) {
        char extra; struct Word w;
        if (count >= ACNH_FPS_PATCH_COUNT || sscanf(line, "%llx %x %x %c", &w.offset, &w.before, &w.after, &extra) != 3
            || (w.offset & 3) || m->file_size < 4 || w.offset > m->file_size - 4 || w.offset >= m->key_off
            || (count && w.offset <= words[count - 1].offset)) runtime_fail("Invalid FPS patch range/order");
        unsigned actual; memcpy(&actual, m->base + w.offset, 4);
        if (actual != w.before) {
            fprintf(stderr, "[acnh-fps] main+%#llx expected %08x, found %08x\n", w.offset, w.before, actual);
            runtime_fail("FPS patch original instruction mismatch");
        }
        words[count++] = w;
    }
    if (count != ACNH_FPS_PATCH_COUNT) runtime_fail("FPS patch count mismatch");
    /* Validate every site before changing the first. The loader seals/flushes
     * instruction-cache pages afterwards, before any guest thread executes. */
    for (unsigned i = 0; i < count; i++) memcpy(m->base + words[i].offset, &words[i].after, 4);
    free(words); free(data);
    fprintf(stderr, "[acnh-fps] verified and installed %u main-module speed corrections\n", count);
#else
    runtime_fail("60 FPS unsupported in this build");
#endif
}
