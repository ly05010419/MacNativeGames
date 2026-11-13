/* Compute dispatches ordered by the game's own barriers (included by nvn_mtl.m before the copy encoder helpers; driven by
   disp_enc and dispatch there). Port of nxnative 547c5db6, "nvn/dispatch: order compute dispatches by the game's barriers"
   (PR #2250; seedds, FifteenPen; MIT).

   NVN makes a dispatch's writes visible to later GPU work only when the game asks with nvnCommandBufferBarrier, so two
   dispatches the game did not separate by a barrier may run at the same time. disp_enc put a barrier before every dispatch
   in the shared encoder (g_blit), so the GPU ran them one at a time. With DM_NVN_DISPATCH_BARRIER=game a dispatch in that
   encoder gets a barrier only when
   - the game issued a barrier since the dispatch before it (OP_BARRIER with bits, oot_bar_native), or an ordering point
     the host does not see through: an event wait (guest queue ordering may stop there and put another queue's work in
     between) or a new top-level command list (mtl_exec). Upstream does not mark these two; they only add barriers;
   - the encoder holds work of unknown reach since its last barrier: a host operation (copies, uploads, uniform updates,
     fills and the host's helper dispatches all go through blit_kind) or a dispatch kept in the full order;
   - it has a binding whose reach is no memory range (cs_rangeless, nvn_mtl.m): the full order on both sides, as upstream.
   Upstream checks copies and dispatches against each other's ranges (g_bw / g_br for copies, g_dw / g_dr for dispatches).
   Here every host operation puts a barrier before itself (blit_kind) and records no range, so it counts as writing
   everything, as upstream's blit() does for texture copies and uploads; dispatch range sets would have no reader and are
   not kept. A new encoder starts behind a full queue barrier (dsp_note_encoder_open), as upstream's blit_open empties its sets.

   Mode: DM_NVN_DISPATCH_BARRIER=game | full (full: a barrier before every dispatch, as before; diagnostic). Default game
   for Ocarina of Time on the Mac (where OOT_DRAW_BARRIERS already orders draw passes by the game's barriers), full for
   every other game until it is measured there. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <TargetConditionals.h>

#ifndef DSP_BARRIER_DEFAULT
#if defined(GAME_OOT) && !TARGET_OS_IPHONE
#define DSP_BARRIER_DEFAULT 1
#else
#define DSP_BARRIER_DEFAULT 0
#endif
#endif

static int g_dsp_game_bar = 1;   // a game barrier or an ordering point since the last dispatch in the shared encoder
static int g_dsp_unknown;        // the shared encoder holds work of unknown reach since its last barrier
static uint64_t g_dsp_host_ops;  // host operations put into the shared encoder (blit_kind)
static uint64_t g_dbar_game, g_dbar_host, g_dbar_full, g_dbar_free, g_dbar_late;   // per dispatch, game mode (periodic [nvn] line)

static int dbar_game(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_DISPATCH_BARRIER");
        v = e && !strcmp(e, "game") ? 1 : e && !strcmp(e, "full") ? 0 : DSP_BARRIER_DEFAULT;
        MLOG("compute dispatches: %s (DM_NVN_DISPATCH_BARRIER=%s%s)", v ? "ordered by the game's barriers" : "each ordered after everything before it in its encoder",
             v ? "game" : "full", e ? "" : ", this game's default");
    }
    return v;
}
static void dsp_note_game_barrier(void) { g_dsp_game_bar = 1; }
static void dsp_note_host_op(void) { g_dsp_unknown = 1; g_dsp_host_ops++; }
static void dsp_note_encoder_open(void) { g_dsp_unknown = 0; }
/* Whether a dispatch about to go into the shared encoder needs a barrier before it. open: the encoder already holds work
   (a new one starts behind its own queue barrier). rangeless: one of its bindings has no memory range. */
static int dsp_barrier_before(int open, int rangeless) {
    int bar;
    if (!dbar_game()) bar = open;
    else if (rangeless) { bar = open; g_dbar_full++; }
    else {
        bar = open && (g_dsp_game_bar || g_dsp_unknown);
        if (!bar) g_dbar_free++; else if (g_dsp_game_bar) g_dbar_game++; else g_dbar_host++;
    }
    g_dsp_game_bar = 0;
    if (bar) g_dsp_unknown = 0;
    g_dsp_unknown |= rangeless;   // what comes after it is ordered after it
    return bar;
}
