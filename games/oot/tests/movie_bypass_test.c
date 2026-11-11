/* #2037 / #2039 / #2042: the title and opening movies' network (oot_movie_bypass.h), recognised by what each dispatch reads
   and writes, as the replay sees a movie frame (passdump F1045, 2026-11-11): two buffer copies fill the decoded planes, a
   dispatch converts them into T, twelve network dispatches (the last writes N at T's size), and the upscale samples N
   (not T: OOT_MOVIE_LOG, f147) and writes the bigger canvas O. */
#include <stdio.h>
#include <string.h>
#include "../third_party/d2r_gfx/gfx/nvn/oot_movie_bypass.h"

static int fails;
static void check(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); fails++; } }

static const char *const NET[] = {"ba079f68ae288dba", "a8b4fb48fee0ccf8", "e2eebd7ba18fe35f", "e6be82d835fbe4d3", "9442573ee4ace011",
                                  "0a56002cdc7e1fce", "0a56002cdc7e1fce", "d15370a382630d4e", "e6be82d835fbe4d3", "7f7ff0f36b54d90f",
                                  "a8b4fb48fee0ccf8", "5a7d69b067a7f011"};
enum { NNET = sizeof NET / sizeof NET[0], T = 6548, NETOUT = 6549, O = 6734 };

/* One movie frame; returns how many network dispatches were skipped and the upscale's answer in *up. */
static int movie_frame(mvbp *m, uint64_t f, int *up) {
    mvbp_note_upload(m, f, 6732); mvbp_note_upload(m, f, 6733);
    mv_disp conv = {"49dff010b9832251", mvbp_uploaded(m, f, 6732), T, 2023, 1920, 1080};
    check(mvbp_step(m, f, &conv) == MV_RUN, "the conversion runs");
    int skipped = 0;
    for (int i = 0; i < NNET; i++) {
        mv_disp d = {NET[i], 0, i == NNET - 1 ? NETOUT : 0, i == NNET - 1 ? 2027 : 0, 1920, 1080};
        int r = mvbp_step(m, f, &d); check(r == MV_RUN || r == MV_SKIP, "a network dispatch runs or is skipped"); skipped += r == MV_SKIP;
    }
    mv_disp u = {"4f41ccf24f163e0d", 0, O, 2013, 2560, 1440};
    *up = mvbp_step(m, f, &u);
    return skipped;
}

int main(void) {
    mvbp m; mvbp_init(&m);
    int up;
    /* frame 1: learns; nothing skipped, the upscale is found (the caller gets a scaler ready) */
    check(movie_frame(&m, 1, &up) == 0, "the first movie frame skips nothing");
    check(up == MV_UPSCALE && !mvbp_ready(&m), "the first frame finds the upscale and is not ready yet");
    check(mvbp_t_tid(&m) == 2023, "T's texture id is kept for the caller's MetalFX input");
    mvbp_learned(&m);
    check(mvbp_ready(&m) && m.nchain == NNET, "the chain between the conversion and the upscale is learned");
    /* frame 2: every network dispatch skipped, the upscale replaced */
    check(movie_frame(&m, 2, &up) == NNET && up == MV_UPSCALE, "the next frame skips the network and replaces the upscale");
    /* a gameplay frame: no upload, nothing is the movie's */
    mv_disp g = {"a8b4fb48fee0ccf8", 0, 0, 0, 0, 0};
    check(mvbp_step(&m, 3, &g) == MV_NONE, "a dispatch of a frame without a movie conversion is not the movie's");
    /* a dispatch with the conversion's key but no uploaded input is not a conversion */
    mv_disp fake = {"49dff010b9832251", 0, T, 2023, 1920, 1080};
    check(mvbp_step(&m, 4, &fake) == MV_NONE, "a conversion is told by its uploaded input, not by its key");
    /* a dispatch the learned chain does not have runs */
    mvbp_note_upload(&m, 5, 6732);
    mv_disp conv = {"49dff010b9832251", 1, T, 2023, 1920, 1080};
    check(mvbp_step(&m, 5, &conv) == MV_RUN, "conversion");
    mv_disp other = {"1111111111111111", 0, 0, 0, 0, 0};
    check(mvbp_step(&m, 5, &other) == MV_RUN, "a dispatch outside the learned chain runs");
    /* the same size: not an upscale (5a7d writes the network output at T's size) */
    mv_disp same = {"5a7d69b067a7f011", 0, NETOUT, 2027, 1920, 1080};
    check(mvbp_step(&m, 5, &same) != MV_UPSCALE, "an image of T's size is not the upscale");
    mv_disp wide = {"2222222222222222", 0, 7000, 3000, 2560, 1080};
    check(mvbp_step(&m, 5, &wide) != MV_UPSCALE, "an image larger in one size only is not the upscale");
    /* the encode failed: not ready, the next movie frame learns again */
    mvbp_failed(&m);
    check(!mvbp_ready(&m), "a failed encode turns the replacement off until it is ready again");
    check(movie_frame(&m, 6, &up) == 0 && up == MV_UPSCALE, "after a failure the next frame runs the network and learns again");
    /* switch off */
    mvbp_learned(&m); mvbp_off(&m);
    check(movie_frame(&m, 7, &up) == 0, "with the switch off nothing is skipped");
    if (fails) return 1;
    puts("movie bypass: all checks pass");
    return 0;
}
