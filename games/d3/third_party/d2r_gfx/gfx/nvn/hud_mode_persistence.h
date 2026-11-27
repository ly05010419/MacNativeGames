/* Mac HUD preference, beside this game's controls.txt. No profile means no
   persistence (standalone probes must never touch the user's preferences).
   Included by fps_overlay.c after its platform and C library headers. */
#if defined(__APPLE__) && TARGET_OS_OSX
#include <unistd.h>
static pthread_mutex_t hud_mode_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t hud_mode_once = PTHREAD_ONCE_INIT;
static char hud_mode_path[4096];
static void hud_mode_load(void) {
    const char *controls = getenv("TOTK_CONTROLS");
    if (!controls || !*controls) return;
    const char *slash = strrchr(controls, '/');
    int n = slash ? snprintf(hud_mode_path, sizeof hud_mode_path, "%.*s/hud-mode.txt", (int)(slash-controls), controls)
                  : snprintf(hud_mode_path, sizeof hud_mode_path, "hud-mode.txt");
    if (n < 0 || (size_t)n >= sizeof hud_mode_path) { hud_mode_path[0] = 0; return; }
    FILE *f = fopen(hud_mode_path, "r");
    if (!f) return;
    int mode; char extra;
    if (fscanf(f, "%d %c", &mode, &extra) == 1 &&
        (mode == HUD_MODE_OFF || mode == HUD_MODE_ALL || mode == HUD_MODE_MINIMAL))
        atomic_store_explicit(&g_hud_mode, mode, memory_order_relaxed);
    fclose(f);
}
static void hud_mode_save(int mode) {
    if (!hud_mode_path[0]) return;
    char tmp[sizeof hud_mode_path + 16];
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", hud_mode_path);
    int fd = mkstemp(tmp);
    if (fd < 0) { fprintf(stderr, "[hud] cannot save HUD preference\n"); return; }
    char value[3] = {(char)('0' + mode), '\n', 0};
    int ok = write(fd, value, 2) == 2;
    if (close(fd)) ok = 0;
    if (ok && rename(tmp, hud_mode_path) == 0) return;
    unlink(tmp);
    fprintf(stderr, "[hud] cannot save HUD preference\n");
}
#define HUD_MODE_INIT() pthread_once(&hud_mode_once, hud_mode_load)
#define HUD_MODE_LOCK() pthread_mutex_lock(&hud_mode_lock)
#define HUD_MODE_UNLOCK() pthread_mutex_unlock(&hud_mode_lock)
#else
#define HUD_MODE_INIT() ((void)0)
#define HUD_MODE_LOCK() ((void)0)
#define HUD_MODE_UNLOCK() ((void)0)
static void hud_mode_save(int mode) { (void)mode; }
#endif
