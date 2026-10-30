/* TOTK_TIMELINE=<file>:<first frame>:<frames> (diagnostic, off by default): one timeline of a run of frames on one clock,
   to see which link of the chain "game logic -> submit -> replay -> GPU -> the game waiting for that frame" sets the frame
   time. Host events are CLOCK_UPTIME_RAW nanoseconds; GPU start / end of each committed command buffer come from Metal's
   commit feedback (seconds on the same mach clock). Frames are counted by the game's presents (nvnQueuePresentTexture).
   The events go to a preallocated buffer and are written to <file> once, a few frames after the window
   (scripts/diag/timeline.py reads it). */
#pragma once
#include <stdint.h>

enum {
    TL_SUB_IN = 1, TL_SUB_OUT,      /* nvnQueueSubmitCommands enter / return (a = draws so far is not known: 0) */
    TL_REP_BEGIN, TL_REP_END,       /* replay of a submission (submit_exec), on the game thread or the translation thread */
    TL_COMMIT,                      /* a command buffer committed: a = commit id, b = 1 last of its submission, 0 mid-submission */
    TL_GPU,                         /* GPU span of commit a: t = start, b = end (ns) */
    TL_WAIT_IN, TL_WAIT_OUT,        /* nvnSyncWait that blocks: a = fence value waited for, b = 1 fence of the newest commit */
    TL_ACQ_IN, TL_ACQ_OUT,          /* nvnWindowAcquireTexture */
    TL_PRES_IN, TL_PRES_OUT,        /* nvnQueuePresentTexture */
    TL_FINISH_IN, TL_FINISH_OUT,    /* nvnQueueFinish */
    TL_FENCE,                       /* fence value of commit a: b */
    TL_RS_JOIN,                     /* the replay thread waited a ns for the encode stage (b = join count) */
    TL_PASS_BEGIN, TL_PASS_END,     /* a render pass opened / ended on the replay thread: a = pass serial, b = draws in it (end) */
    TL_MARGIN,                      /* (task 0n) a present committed on the display link: a = microseconds before its target
                                       refresh (the margin), b = microseconds from the game's enqueue to that refresh (0 if unknown) */
};

int totk_tl_on(void);                                   /* the window is open (cheap after the first call) */
void totk_tl(int kind, uint64_t a, uint64_t b);         /* event now, on this thread */
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b);
void totk_tl_frame(uint64_t presents);                  /* the game presented: counts frames, opens / closes the window, writes the file */
uint64_t totk_tl_commit_id(void);                       /* id for a command buffer about to be committed */
