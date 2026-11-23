/* Pacing of a host movie decoder's frames (runtime/movie_host.c): where its presentation clock starts again.
   movie_host.c hands decoded frames to the game when their presentation time is due, counted from an anchor (the host
   time and the pts of the first frame handed over). Flush and Stop drop the anchor; a game that seeks without either,
   or a movie that loops, does not. Mario Kart 8 Deluxe's mode-menu preview is one H.264 file of clips: A on Grand Prix
   moves its reader from ~10 s to the engine-class clip at 144 s with no Flush, and the game opens the engine-class menu
   only when that clip's first frame comes out. Counted from the old anchor that frame was due 134 s later, so the menu
   waited "a minute or two" with every output slot held (2026-11-22). A pts that goes backwards or jumps further than
   any frame interval a video has is a new run of the stream: the pacing starts again from it.
   Shared by every port whose movie_host.c paces frames (it needs nothing but <stdint.h>). */
#ifndef MOVIE_PACE_H
#define MOVIE_PACE_H
#include <stdint.h>

/* Longest gap between two consecutive frames that is still the same run (us). Real frame intervals are 1/60 to ~1/10
   s; a still held for longer than this in a variable-rate stream is shown a little early, nothing worse. */
#define MOVIE_PACE_MAX_GAP_US 1000000

/* 1 when the frame at `pts` does not follow the frame at `prev_pts` in the same run (a seek or a loop). */
static inline int movie_pace_jump(int64_t prev_pts, int64_t pts) {
    return pts < prev_pts || pts - prev_pts > MOVIE_PACE_MAX_GAP_US;
}
#endif
