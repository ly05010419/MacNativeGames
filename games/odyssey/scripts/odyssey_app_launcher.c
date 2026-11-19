/* Odyssey.app launcher (scripts/package_native_app.py builds the bundle around it).
   Everything the game reads is inside the bundle (Contents/Resources/game: runtime, shader tools, rewritten modules,
   RomFS, shared fonts); everything it writes is in the player's data folder (saves, shader cache, log), so the
   app keeps working when the project checkout is not there. Contents/MacOS/odyssey-native is a symlink to the bundled
   runtime: started through it, the window belongs to this app (name and icon), while the runtime still finds its shader
   tools next to the real file. The game runs as the user's (TOTK_SLOT_OWNER=user), so it takes the one-game slot ahead
   of test runs. "session-minutes.txt" in the data folder sets a session limit (default 20, 0 = none): the copied renderer
   has caused GPU driver panics in long sessions, so sessions stay bounded until longer runs are verified.

   The plumbing every port's launcher shares (the bundle, the data folder and its move, backups, the common environment,
   logs, the run loop) is app_launcher.h, a link into shared/app/; what follows is this game's own. */
#include <signal.h>
#include "app_launcher.h"

/* The session limit (minutes, 0 = none) in session-minutes.txt, read again while the game runs: the in-game settings
   panel's row (runtime/settings_game.h) changes it during play. There is no window before the game any more (user
   2026-11-17: "去掉这个 都放在设置里去设置"); every setting is in the panel (key 1). */
static double session_minutes(const char *path) {
    double minutes = 20; FILE *mf = fopen(path, "r");
    if (mf) { if (fscanf(mf, "%lf", &minutes) != 1 || minutes < 0) minutes = 20; fclose(mf); }
    return minutes;
}
static void append(const char *path, const char *line) {
    FILE *f = fopen(path, "a"); if (f) { fputs(line, f); fclose(f); }
}

/* Waiting for the game with the session limit: at the limit the game is asked to close (commands.txt "close"). The
   game finishes a save in progress before quitting (up to 15 s, frontend.m windowWillClose); only a game that has not
   quit well after that is stopped. A crash, not the session limit, keeps its log apart from the next session's. */
struct session { const struct launcher_data *data; char minutes_path[PATH_MAX]; };
static int wait_with_session_limit(pid_t child, const char *stamp, void *context) {
    const struct session *s = context; const struct launcher_data *d = s->data;
    double minutes = session_minutes(s->minutes_path);
    time_t start = time(NULL), asked = 0;
    for (;;) {
        int status; pid_t r = waitpid(child, &status, WNOHANG);
        if (r == child) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (code && !asked) {
                char crash[PATH_MAX], name[96]; snprintf(name, sizeof name, "crash-%s.log", stamp);
                launcher_join(crash, d->data, name); rename(d->log, crash);
            }
            return code;
        }
        if (r < 0 && errno != EINTR) return 1;
        time_t now = time(NULL);
        if (!asked && (long)difftime(now, start) % 5 == 0) minutes = session_minutes(s->minutes_path);
        if (minutes > 0 && !asked && difftime(now, start) >= minutes * 60) {
            char note[160]; snprintf(note, sizeof note, "[Odyssey.app] %g-minute session limit: closing the game\n", minutes);
            append(d->log, note); append(d->commands, "close\n"); asked = now;
        }
        if (asked && difftime(now, asked) >= 45) kill(child, SIGTERM);
        if (asked && difftime(now, asked) >= 60) kill(child, SIGKILL);
        sleep(1);
    }
}

int main(void) {
    struct launcher_bundle b;
    if (launcher_find_bundle(&b)) return 1;
    char runtime[PATH_MAX], modules[PATH_MAX], romfs[PATH_MAX], fonts[PATH_MAX];
    launcher_join(runtime, b.macos, "odyssey-native"); launcher_join(modules, b.game, "rewritten/local");
    launcher_join(romfs, b.game, "romfs"); launcher_join(fonts, b.game, "fonts");
    char source_path[PATH_MAX], source[64] = ""; launcher_join(source_path, b.resources, "source-commit.txt");
    FILE *source_file = fopen(source_path, "r");
    if (source_file) { if (fscanf(source_file, "%63s", source) == 1) setenv("TOTK_SOURCE_COMMIT", source, 1); fclose(source_file); }

    /* ~/Library/Application Support/MacNativeGames/Odyssey (scripts/package_native_app.py DATA; MNG Launcher's Storage and
       Saves pages look there). Until 2026-11-17 it was "Odyssey Data" next to the app: a folder found there is moved once,
       never over data already in the new place. */
    char folder[PATH_MAX]; struct launcher_data d;
    if (launcher_data_folder(folder, &b, "Odyssey", NULL, "Odyssey Data", "Odyssey")) return 1;
    launcher_data_files(&d, folder);
    char cache[PATH_MAX], gfx_path[PATH_MAX];
    launcher_join(cache, d.data, "shader-cache/rt-totk-native-v4-unverified");
    launcher_join(gfx_path, d.data, "graphics.txt");
    struct session session = {.data = &d};
    launcher_join(session.minutes_path, d.data, "session-minutes.txt");

    /* The project's slot directory when the app was packaged there (so test runs and the app never overlap),
       otherwise one next to the data. */
    char slot[PATH_MAX] = "", slot_file[PATH_MAX]; launcher_join(slot_file, b.game, "slot-dir.txt");
    FILE *sf = fopen(slot_file, "r");
    if (sf) { if (fgets(slot, sizeof slot, sf)) slot[strcspn(slot, "\n")] = 0; fclose(sf); }
    struct stat st;
    if (!*slot || stat(slot, &st) || !S_ISDIR(st.st_mode)) { launcher_join(slot, d.data, "game-slot"); mkdir(slot, 0755); }

    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 1); setenv("TOTK_GPU_WIDEN", "1", 1); setenv("TOTK_GPU_BLOCKRT", "1", 1);
    launcher_common_env(&d, cache, romfs, fonts, slot, "Odyssey.app");
    /* Key 3's bigger render size (build_odyssey.py --render-res): the scaled graphics table and render-res.txt in the
       bundle; without that folder the game runs as shipped. */
    { char patches[PATH_MAX]; launcher_join(patches, b.game, "romfs-patches");
      setenv("TOTK_ROMFS_PATCHES", stat(patches, &st) == 0 && S_ISDIR(st.st_mode) ? patches : "0", 0); }
    /* The update's version the app was built from (package_native_app.py), for the window title: 1.3.0 and 1.4.1 share
       their executables, so the runtime itself cannot tell them apart. */
    { char path[PATH_MAX], version[32] = ""; launcher_join(path, b.resources, "game-version.txt");
      FILE *vf = fopen(path, "r");
      if (vf) { if (fgets(version, sizeof version, vf)) version[strcspn(version, "\r\n")] = 0; fclose(vf); }
      if (*version) setenv("ODYSSEY_GAME_VERSION", version, 1);
      /* The player's RomFS mods (runtime/romfs.c, issue #1788): <data>/mods/<version>, named by the version's digits as
         the Zelda apps name theirs (v130, v141). The runtime loads the folder when it holds a manifest.tsv
         (scripts/make_mod_manifest.py); without one the log says "RomFS overlay: none" and names the folder. An app
         without game-version.txt is 1.3.0's (runtime/game_version.h). Both updates merge to the same RomFS, so a
         manifest written for one fits the other. A TOTK_ROMFS_MODS given to the app decides first (0 = none). */
      char digits[32] = ""; size_t n = 0;
      for (const char *c = version; *c && n + 1 < sizeof digits; c++) if (*c >= '0' && *c <= '9') digits[n++] = *c;
      digits[n] = 0;
      char name[48], mods[PATH_MAX]; snprintf(name, sizeof name, "mods/v%s", n ? digits : "130");
      launcher_join(mods, d.data, name);
      setenv("TOTK_ROMFS_MODS", mods, 0); }
    /* The player's button layout (runtime/controller_map.h), when controls.txt exists in the data folder. A TOTK_CONTROLS
       given to the app decides first (the other ports' launchers set theirs over it). */
    setenv("TOTK_CONTROLS", d.controls, 0);
    /* How GameController's A/B/X/Y map to positions (runtime/gamepad_layout.h, TOTK_FACE_BUTTONS): "position" or "label"
       in face-buttons.txt. "position" makes an Xbox-style controls.txt (south = a ...) match the printed letters on a
       Nintendo controller too (24 October: a Pro Controller had A and B swapped with that file). */
    char face_path[PATH_MAX]; launcher_join(face_path, d.data, "face-buttons.txt");
    FILE *ff = fopen(face_path, "r");
    if (ff) { char mode[16] = ""; if (fscanf(ff, "%15s", mode) == 1 && (!strcmp(mode, "position") || !strcmp(mode, "label"))) setenv("TOTK_FACE_BUTTONS", mode, 0); fclose(ff); }
    /* A freeze (no frame for 15 s) logs which threads hold and wait for the virtual cores (runtime/freeze_watchdog.c),
       so a hang in play can be diagnosed afterwards (23 October: a world-travel hang left no such record). */
    /* The game's language follows the Mac's first preferred language (runtime/services.c reads TOTK_LOCALE): its own
       fallback, the current locale, is the app bundle's English inside the app (2026-11-18, as Animal Crossing). */
    launcher_set_locale();
    setenv("ODYSSEY_FREEZE_WATCHDOG", "15", 0);
    /* Shader variants and pipelines the game met that the app's list lacks (docs/shader-list.md), sent to the
       shader inbox when the settings panel's switch is on (totk_contrib_upload.h): identifiers only. Without this the runtime writes no contribution file at all. */
    char contrib[PATH_MAX]; launcher_join(contrib, d.data, "shader-list-contribution.txt");
    setenv("TOTK_SHADER_CONTRIB", contrib, 0);
    /* MetalFX spatial upscale of the 1600x900 game output to the window (Applier's A/B, 22 October: no artifacts, no
       measurable cost, slightly crisper edges). On unless "no-metalfx" exists in the data folder. */
    char no_mfx[PATH_MAX]; launcher_join(no_mfx, d.data, "no-metalfx");
    if (stat(no_mfx, &st)) { setenv("TOTK_MFX_UPSCALE", "1", 0); setenv("TOTK_MFX_OUTPUT", "1", 0); }
    /* Full screen from the start (Applier's 0c A/B, 22 October: input-to-present latency p50 63 -> 30 ms, because the
       windowed compositor path adds ~34 ms of display-link lead). ⌃⌘F or the green button leaves it. Windowed when
       "windowed" exists in the data folder. */
    char windowed[PATH_MAX]; launcher_join(windowed, d.data, "windowed");
    char display_path[PATH_MAX], display[16] = ""; launcher_join(display_path, d.data, "display.txt");   /* the settings panel's Display row */
    FILE *df = fopen(display_path, "r"); if (df) { if (fscanf(df, "%15s", display) != 1) display[0] = 0; fclose(df); }
    if (strcmp(display, "window") && (!strcmp(display, "fullscreen") || stat(windowed, &st))) setenv("BY3_FULLSCREEN", "1", 0);
    /* The in-game panel and the launcher's graphics page share this file. Keep the build's largest verified canvas
       and heap allocation so the scene cap can change live; no launch setting forces a runtime override. */
    setenv("ODYSSEY_GFX_SETTINGS", gfx_path, 0);

    /* Backups named by the start's time alone (the crash log takes the same name). */
    struct launcher_play play = {.name = "Odyssey", .data = &d, .runtime = runtime, .argv0 = "odyssey-native",
                                 .modules = modules, .stamp_with_pid = 0, .wait = wait_with_session_limit, .context = &session};
    return launcher_play(&play);
}
