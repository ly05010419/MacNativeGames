/* The controller state the game reads, recorded and replayed (2026-11-18, user: "不是让你录制了吗 我的路线和操作",
   "用我的 路线 运行"): a test plays the user's own route and actions again.

   OOT_INPUT_RECORD=<file>: each change of the state pad_states hands the game (buttons and the four stick axes, after the
   keyboard, the controller and the input file are merged) is a line, by frame. Frames are counted in two segments so a
   load that takes longer on a later run does not shift what follows: B, game frames presented since the start
   (dm_nvn_frame_count), until the world is entered; W, frames since the first world frame (mtl_world_frame, the
   gate hle_nvn.c uses for its warm-up wait). The state is written again when the segment changes, and a heartbeat line
   every 600 frames marks how far the recording went.
     <B|W> <frame> <buttons hex> <lx> <ly> <rx> <ry>      a state, from this frame on
     H <B|W> <frame>                                       the recording reached this frame
   OOT_INPUT_REPLAY=<file>: the game reads the recorded state instead (the real keyboard and controller are ignored),
   and 120 frames after the recording's last line the game closes normally (the input file's "close"; on the Mac only:
   the iOS frontend's close only logs, so there the last state stays until the user closes the game).

   The game itself is not deterministic (its own timing, loading), so a replay follows the route closely, not exactly.

   Shared (2026-11-26): Tears of the Kingdom and Breath of the Wild record and replay the same way, for A/B runs on a route
   the user played. TOTK_INPUT_RECORD / TOTK_INPUT_REPLAY are the names in every game; the OOT_ names still work.
   Breath of the Wild 1.9.0 above 30 fps steps its game time by each frame's measured length, so the same frames hold a
   key for a different time in every run and the route drifts (2026-11-26: replays left the recorded path). Pin the step
   while recording and while replaying: the input file's "framestep 8.333" (1/120 s a frame), sent again once the world
   is entered. With it six replays of one route each reached its end on the same frames. */
#pragma once
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

__attribute__((weak)) uint64_t dm_nvn_frame_count(void);   /* gfx/nvn/hle_nvn.c */
__attribute__((weak)) uint64_t mtl_world_frame(void);      /* gfx/nvn/nvn_mtl.m: the first world frame, 0 before */
void runtime_front_request_close(void);                    /* frontend.m */

typedef struct { char seg; uint64_t frame, buttons; int32_t lx, ly, rx, ry; } TapeEvent;
static pthread_mutex_t g_tape_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_tape_mode = -1;                 /* 0 off, 1 record, 2 replay */
static FILE *g_tape_out;
static TapeEvent g_tape_last; static int g_tape_have_last; static char g_tape_last_seg; static uint64_t g_tape_beat;
static TapeEvent *g_tape; static size_t g_tape_n;
static char g_tape_end_seg; static uint64_t g_tape_end_frame; static int g_tape_closed;

static void tape_where(char *seg, uint64_t *frame) {
    uint64_t now = dm_nvn_frame_count ? dm_nvn_frame_count() : 0, w = mtl_world_frame ? mtl_world_frame() : 0;
    if (w && now >= w) { *seg = 'W'; *frame = now - w; } else { *seg = 'B'; *frame = now; }
}
static void tape_open(void) {
    const char *rec = getenv("TOTK_INPUT_RECORD"), *rep = getenv("TOTK_INPUT_REPLAY");
    if (!rec || !*rec) rec = getenv("OOT_INPUT_RECORD");
    if (!rep || !*rep) rep = getenv("OOT_INPUT_REPLAY");
    g_tape_mode = 0;
    if (rep && *rep) {
        FILE *f = fopen(rep, "r");
        if (!f) { fprintf(stderr, "[input-tape] 【红】cannot read %s: no replay\n", rep); return; }
        size_t cap = 1024; g_tape = (TapeEvent *)malloc(cap * sizeof *g_tape);
        char line[256];
        while (g_tape && fgets(line, sizeof line, f)) {
            TapeEvent e; char seg; unsigned long long fr, b;
            /* the file is in the order it was played: its last line, a state or a heartbeat, is where the recording ended */
            if (line[0] == 'H' && sscanf(line, "H %c %llu", &seg, &fr) == 2) { g_tape_end_seg = seg; g_tape_end_frame = fr; continue; }
            if (sscanf(line, "%c %llu %llx %d %d %d %d", &seg, &fr, &b, &e.lx, &e.ly, &e.rx, &e.ry) != 7 || (seg != 'B' && seg != 'W')) continue;
            e.seg = seg; e.frame = fr; e.buttons = b;
            if (g_tape_n == cap) { TapeEvent *g = (TapeEvent *)realloc(g_tape, (cap *= 2) * sizeof *g_tape); if (!g) break; g_tape = g; }
            g_tape[g_tape_n++] = e;
            g_tape_end_seg = seg; g_tape_end_frame = fr;
        }
        fclose(f);
        if (!g_tape_n) { fprintf(stderr, "[input-tape] 【红】%s holds no state: no replay\n", rep); return; }
        size_t w = 0; for (size_t i = 0; i < g_tape_n; i++) w += g_tape[i].seg == 'W';
        fprintf(stderr, "[input-tape] replaying %s: %zu states before the world, %zu in it, to %c %" PRIu64 "\n", rep, g_tape_n - w, w, g_tape_end_seg, g_tape_end_frame);
        g_tape_mode = 2;
    } else if (rec && *rec) {
        g_tape_out = fopen(rec, "w");
        if (!g_tape_out) { fprintf(stderr, "[input-tape] 【红】cannot write %s: no recording\n", rec); return; }
        fprintf(g_tape_out, "# controller states (shared/runtime/input_tape.h): <B|W> <frame> <buttons> <lx> <ly> <rx> <ry>\n");
        fflush(g_tape_out);
        fprintf(stderr, "[input-tape] recording the controller to %s\n", rec);
        g_tape_mode = 1;
    }
}
/* pad_states, with the state it is about to hand the game */
static void input_tape(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    pthread_mutex_lock(&g_tape_mu);
    if (g_tape_mode < 0) tape_open();
    if (!g_tape_mode) { pthread_mutex_unlock(&g_tape_mu); return; }
    char seg; uint64_t frame; tape_where(&seg, &frame);
    if (g_tape_mode == 1) {
        TapeEvent e = { seg, frame, *buttons, *lx, *ly, *rx, *ry };
        int changed = !g_tape_have_last || seg != g_tape_last_seg || e.buttons != g_tape_last.buttons || e.lx != g_tape_last.lx ||
                      e.ly != g_tape_last.ly || e.rx != g_tape_last.rx || e.ry != g_tape_last.ry;
        if (changed) {
            fprintf(g_tape_out, "%c %" PRIu64 " %" PRIx64 " %d %d %d %d\n", seg, frame, e.buttons, e.lx, e.ly, e.rx, e.ry);
            g_tape_last = e; g_tape_have_last = 1; g_tape_last_seg = seg;
        }
        if (frame / 600 != g_tape_beat || changed) {
            if (frame / 600 != g_tape_beat) { g_tape_beat = frame / 600; fprintf(g_tape_out, "H %c %" PRIu64 "\n", seg, frame); }
            fflush(g_tape_out);   /* a close or a crash keeps what was played */
        }
    } else {
        /* the latest state at or before this point: the tape is in time order and B comes before W, so a cursor moves on */
        static size_t at; static int started;
        while (at + (size_t)started < g_tape_n) {
            const TapeEvent *next = &g_tape[at + (size_t)started];
            if (!(next->seg < seg || (next->seg == seg && next->frame <= frame))) break;
            at += (size_t)started; started = 1;
        }
        const TapeEvent *use = started && g_tape[at].seg == seg ? &g_tape[at] : NULL;   /* in the world, only the world's */
        /* TOTK_INPUT_REPLAY_LIVE=1 (or OOT_INPUT_REPLAY_LIVE=1) (tests that drive the controllers themselves): in the world the live state stays, the tape only adds to it */
        static int live = -1; if (live < 0) { const char *e = getenv("TOTK_INPUT_REPLAY_LIVE"); if (!e || !*e) e = getenv("OOT_INPUT_REPLAY_LIVE"); live = e && *e == '1'; }
        if (live && seg == 'W') { if (use) { *buttons |= use->buttons; if (use->lx) *lx = use->lx; if (use->ly) *ly = use->ly; if (use->rx) *rx = use->rx; if (use->ry) *ry = use->ry; } }
        else if (use) { *buttons = use->buttons; *lx = use->lx; *ly = use->ly; *rx = use->rx; *ry = use->ry; }
        else { *buttons = 0; *lx = *ly = *rx = *ry = 0; }
        if (!g_tape_closed && seg == g_tape_end_seg && frame > g_tape_end_frame + 120) {
            g_tape_closed = 1;
            fprintf(stderr, "[input-tape] the recording ended (%c %" PRIu64 "): closing the game\n", seg, g_tape_end_frame);
            runtime_front_request_close();
        }
    }
    pthread_mutex_unlock(&g_tape_mu);
}
