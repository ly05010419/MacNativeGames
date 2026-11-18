/* Scripted input for diagnostics and tests, the same in every game (shared/README.md, 2026-11-18): TOTK_INPUT_SCRIPT
   presses keys at given times, and TOTK_INPUT_FILE is a command stream read as it grows. Included once, at the end of
   each game's runtime/input.c, which keeps its own key table (runtime_input_key_synthetic) and defines first:
     static int input_game_command(char *line)          1: a command of this game's own (its list is there), done
     static int input_game_flag(const char *name, int v)  1: a "flag" this game switches itself
   Commands every game has: key <Mac keycode> <milliseconds>, down/up <Mac keycode>, shot <path>, shotn <n> <path with %d>,
   capture, wshot <path>, texdump <path with %d>, passdump <frames>, querylog <frames>, anomdump, flag <name> <0|1>,
   kbtest / kbtype <text> / kbenter / kbesc / kbshot <path> (the text entry bar), threads (dump every guest thread's wait),
   hletrace <0|1|2> (host calls the game makes), framestep <ms> (pin the frame step), mscan ... (memory scan and write
   watch, runtime/mem_scan.h, where the game has it), close. */
#ifndef INPUT_FILE_H
#define INPUT_FILE_H
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* TOTK_INPUT_SCRIPT="sec:keycode:ms,...": press Mac key codes at given seconds after start (diagnostics without a
   picture: does the game react to a button). Key codes go through the fixed script layout (J=38 is A, Return=36 is Plus). */
static void *input_script(void *arg) {
    const char *p = arg; uint64_t t0 = runtime_now_ns();
    while (p && *p) {
        double at = strtod(p, (char **)&p); if (*p++ != ':') break;
        unsigned code = (unsigned)strtoul(p, (char **)&p, 10); if (*p++ != ':') break;
        unsigned ms = (unsigned)strtoul(p, (char **)&p, 10); if (*p == ',') p++;
        while ((runtime_now_ns() - t0) / 1e9 < at) usleep(10000);
        fprintf(stderr, "[totk-input] script: key %u down for %u ms at %.1f s\n", code, ms, at);
        runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
    }
    return NULL;
}
__attribute__((constructor)) static void input_script_start(void) {
    const char *e = getenv("TOTK_INPUT_SCRIPT"); if (!e) return;
    pthread_t t; pthread_create(&t, NULL, input_script, (void *)e); pthread_detach(t);
}

/* Optional local integration-test command stream. Appending complete lines lets tests inspect each
   actual frame before choosing the next input, instead of guessing load/menu timings. Disabled normally. */
extern void dm_gfx_shot_at(long frames, const char *path);
extern void runtime_front_request_close(void);
/* The defaults below answer where a game, or a probe that links input.c alone, has no such part. */
/* "mscan ...": memory scan and write watch (runtime/mem_scan.h, in services.c). */
__attribute__((weak)) int runtime_mem_scan_command(const char *line) { (void)line; return 0; }
/* "flag <name> <v>" for the scheduler (threads.c, which overrides this where it has flags). 1 = taken. */
__attribute__((weak)) int runtime_thread_flag(const char *name, int v) { (void)name; (void)v; return 0; }
/* An agent's test game (runtime/game_slot.c in the games that limit them): any command keeps it open. */
__attribute__((weak)) void runtime_slot_activity(int user) { (void)user; }
static void *input_file(void *arg) {
    FILE *f = fopen((const char *)arg, "r"); if (!f) runtime_fail("cannot open TOTK_INPUT_FILE");
    char line[1200];
    for (;;) {
        if (!fgets(line, sizeof line, f)) {
            if (ferror(f)) runtime_fail("diagnostic input read");
            clearerr(f); usleep(100000);
            /* A file replaced instead of appended to (devicectl device copy to an iPhone's Documents writes a new file, so
               this stream would stay on the old one and never see another line): open the new one. The writer copies the
               whole command list again, so it is read on from where the old one ended; a shorter file is a new list, read
               from its start. */
            struct stat now, held; FILE *n;
            if (!stat((const char *)arg, &now) && !fstat(fileno(f), &held) && now.st_ino != held.st_ino && (n = fopen((const char *)arg, "r"))) {
                long at = ftell(f); fclose(f); f = n;
                if (at > 0 && now.st_size >= at) fseek(f, at, SEEK_SET);
            }
            continue;
        }
        runtime_slot_activity(0);   /* any command keeps an agent's game open (a line the reader refuses ends the game) */
        if (runtime_survey_command(line)) continue;
        if (input_game_command(line)) continue;
        unsigned code, ms; int used = 0;
        if (sscanf(line, "down %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u down\n",code);runtime_input_key_synthetic(code,1);
        } else if (sscanf(line, "up %u %n", &code, &used) == 1 && used && !line[used] && code < 128) {
            fprintf(stderr,"[totk-input] file: key %u up\n",code);runtime_input_key_synthetic(code,0);
        } else if (sscanf(line, "key %u %u %n", &code, &ms, &used) == 2 && used && !line[used] && code < 128 && ms > 0 && ms <= 5000) {
            fprintf(stderr, "[totk-input] file: key %u for %u ms\n", code, ms);
            runtime_input_key_synthetic(code, 1); usleep(ms * 1000); runtime_input_key_synthetic(code, 0);
        } else if (sscanf(line, "passdump %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code < 16) {
            extern void dm_gfx_passdump_next(long n); dm_gfx_passdump_next((long)code);
        } else if (!strncmp(line, "anomdump", 8) && (line[8] == '\n' || line[8] == '\r' || !line[8])) {   /* TOTK_ANOMALY=<dir>: write the per-draw records (with GPU sample counts) of the last frames now */
            extern void dm_gfx_anomdump(void); dm_gfx_anomdump();
        } else if (sscanf(line, "querylog %u %n", &code, &used) == 1 && used && !line[used] && code > 0 && code <= 600) {
            extern void dm_gfx_querylog_next(long n); dm_gfx_querylog_next((long)code);   /* occlusion query results of the next n frames */
        } else if (!strncmp(line, "flag ", 5)) {
            /* "flag <name> <0|1>": switch a graphics behaviour while the game runs (A/B without restarting); rumble / gyro mute
               and unmute those; the game's own flags first (input_game_flag), then the scheduler's */
            char name[64]; int v; if (sscanf(line + 5, "%63s %d", name, &v) != 2) runtime_fail("diagnostic flag needs <name> <0|1>");
            extern void dm_gfx_set_flag(const char *name, int v);
            if (!input_game_flag(name, v) && !runtime_hid_feedback_flag(name, v) && !runtime_thread_flag(name, v)) dm_gfx_set_flag(name, v);
        } else if (!strncmp(line, "texdump ", 8)) {
            extern void dm_gfx_texdump_next(const char *path);
            line[strcspn(line, "\r\n")] = 0; if (!line[8] || !strstr(line + 8, "%d")) runtime_fail("diagnostic texdump path needs %d");
            dm_gfx_texdump_next(line + 8);
        } else if (sscanf(line, "shotn %u %n", &code, &used) == 1 && used && code > 0 && code <= 60) {
            /* "shotn <n> <path with %d>": screenshots of n consecutive game frames (frame flicker); %d = 0..n-1 */
            line[strcspn(line, "\r\n")] = 0; const char *fmt = line + used;
            if (!*fmt || !strstr(fmt, "%d") || strchr(strstr(fmt, "%d") + 2, '%')) runtime_fail("diagnostic shotn path needs one %d");
            for (unsigned i = 0; i < code; i++) { char p[1200]; snprintf(p, sizeof p, fmt, (int)i); dm_gfx_shot_at(2 + (long)i, p); }
        } else if (!strncmp(line, "shot ", 5)) {
            line[strcspn(line, "\r\n")] = 0; if (!line[5]) runtime_fail("diagnostic screenshot path");
            dm_gfx_shot_at(2, line + 5);
        } else if (!strncmp(line, "capture", 7) && (!line[7] || line[7] == '\n' || line[7] == '\r')) {   /* "capture": the capture button once (screenshot.h; screenshot sharing tests) */
            if (runtime_capture_press) runtime_capture_press();
        } else if (!strncmp(line, "wshot ", 6)) {   /* "wshot <path>": the next shown picture as the window shows it (FPS box and the bottom-left box included) */
            line[strcspn(line, "\r\n")] = 0; if (!line[6]) runtime_fail("diagnostic window screenshot path");
            extern void dm_gfx_window_shot(const char *path); dm_gfx_window_shot(line + 6);
        } else if (!strcmp(line, "kbtest\n")) {   /* text entry bar the way the game names a horse (runtime/swkbd_host.c) */
            runtime_swkbd_selftest();
        } else if (!strncmp(line, "kbtype ", 7)) {   /* "kbtype <text>": typed into the open bar */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("type", line + 7);
        } else if (!strncmp(line, "kbshot ", 7)) {   /* "kbshot <path>": the open bar as a PNG */
            line[strcspn(line, "\r\n")] = 0; runtime_front_text_test("shot", line + 7);
        } else if (!strcmp(line, "kbenter\n") || !strcmp(line, "kbesc\n")) {
            runtime_front_text_test(line[2] == 'e' && line[3] == 'n' ? "enter" : "esc", NULL);
        } else if (sscanf(line, "hletrace %u %n", &code, &used) == 1 && used && !line[used] && code <= 2) {
            runtime_hle_trace_set((int)code);   /* 1 count host calls, 2 also log the first call of each, 0 stop and print the counts */
        } else if (!strncmp(line, "framestep ", 10)) {   /* "framestep <ms>": pin the real-time frame step, 0 = back to real time */
            char *end; double step = strtod(line + 10, &end); if (end == line + 10 || step < 0 || step > 100) runtime_fail("framestep takes milliseconds (0 = real time)");
            runtime_frame_step_pin(step);
        } else if (!strncmp(line, "mscan ", 6) && runtime_mem_scan_command(line + 6)) {   /* results in memscan.txt */
        } else if (!strcmp(line, "threads\n")) {   /* every guest thread's wait state and guest return chain, now (a hang: what is each thread waiting for) */
            runtime_dump_threads();
        } else if (!strcmp(line, "close\n")) { runtime_front_request_close(); break; }
        else runtime_fail("invalid diagnostic input command");
    }
    fclose(f); return NULL;
}
/* loader.c has the real one where the game is a Mac app the launcher starts; the probes, and the games without, never are. */
__attribute__((weak)) int runtime_app_mode(void) { return 0; }
void runtime_input_file_start(void) {
    static int started; if (started) return;
    const char *e = getenv("TOTK_INPUT_FILE"); if (!e || !*e) return;
    started = 1; pthread_t t; if (pthread_create(&t, NULL, input_file, (void *)e)) runtime_fail("diagnostic input thread"); pthread_detach(t);
}
/* Started as the Mac app, the launcher part (runtime_app_launch) names the file only later: main starts it then. */
__attribute__((constructor)) static void input_file_start(void) { if (!runtime_app_mode()) runtime_input_file_start(); }
#endif
