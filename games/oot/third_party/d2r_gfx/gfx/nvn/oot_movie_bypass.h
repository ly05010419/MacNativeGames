/* OOT (2026-11-11, #2037 / #2039 / #2042): the title and opening movies' network, recognised by data flow so the replay can
   replace it with a MetalFX upscale, always (oot_net_bypass.inc netbp_movie; the user: "默认开，不用设置控制").

   A movie frame, as the replay sees it (passdump F1045 of the title movie Dm_Title_0005, an AV1 video decoded on the host):
     two buffer -> texture copies fill the decoded planes (Y 1920x1080 R8, UV 960x540 RG8)
     one dispatch samples them and writes T, the picture in RGB (49dff010: the BT.709 matrix in its uniforms), 1920x1080
     twelve dispatches of a convolutional network on F16 feature buffers (16 / 8 / 4 MB), the last writing its output image
     at T's size (5a7d69b0)
     the upscale samples the network's output and a 512x512 table, not T, and writes O, the 2560x1440 canvas (4f41ccf2;
     OOT_MOVIE_LOG f147: t0=6357 the network's output, t3=6324 the table, i1=6401 O); only the next render pass reads O
   On an Apple M4 (10 GPU cores) the network and the upscale take about 28 ms of a 29 ms frame, so the 30 fps movie played
   at about 87 % speed while its sound kept the wall clock (skipcs of each, scratch/runs/2026-11/oot-debug/movie-skipcs).

   Recognition, every frame, from what the dispatches read and write (never from their keys):
   - the conversion: a dispatch that samples a texture a buffer copy filled in this frame and writes an image (T);
   - the upscale: a later dispatch of that frame that writes an image larger than T in both sizes (O);
   - the network: the dispatches between them. The first movie frame runs them and records their keys in order; once the
     caller has a scaler ready (mvbp_learned), the next frames skip exactly that sequence, and the caller writes O with
     MetalFX from T. A dispatch that is not next in the recorded sequence runs. A failed MetalFX encode (mvbp_failed) or
     the switch turned off (mvbp_off) makes the next movie frame run the network and record it again. MetalFX reads T, which
   the conversion wrote; the network's output, which only the upscale reads, is not needed then.
   Single-threaded: the replay thread calls everything. tests/movie_bypass_test.c. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { MV_NONE = -1, MV_RUN = 0, MV_SKIP = 1, MV_UPSCALE = 2 };   // mvbp_step: not the movie's / run it / skip it / the upscale
enum { MV_UPLOADS = 8, MV_CHAIN = 32 };

typedef struct {
    const char *key;         // the compute shader's key (16 hex digits used)
    int reads_upload;        // samples a texture a buffer copy filled in this frame (mvbp_uploaded)
    uint64_t image_serial;   // the texture its first image binding writes, 0 none
    int image_tid;           // that texture's id (the caller's handle for T)
    uint32_t img_w, img_h;   // that image's size
} mv_disp;

typedef struct {
    uint64_t up[MV_UPLOADS], up_frame; int nup;   // textures buffer copies filled in frame up_frame
    uint64_t frame, t; uint32_t t_w, t_h; int t_tid;   // the movie frame in progress and its T
    int idx, ncand, overflow, ready, nchain;
    char chain[MV_CHAIN][17], cand[MV_CHAIN][17];
    uint64_t skipped, mismatched;                 // dispatches skipped; replaced frames whose sequence differed from the record
} mvbp;

static inline void mvbp_init(mvbp *m) { memset(m, 0, sizeof *m); m->up_frame = m->frame = UINT64_MAX; }
static inline void mvbp_note_upload(mvbp *m, uint64_t frame, uint64_t serial) {
    if (m->up_frame != frame) { m->up_frame = frame; m->nup = 0; }
    if (m->nup < MV_UPLOADS) m->up[m->nup++] = serial;
}
static inline int mvbp_uploaded(const mvbp *m, uint64_t frame, uint64_t serial) {
    if (!serial || m->up_frame != frame) return 0;
    for (int i = 0; i < m->nup; i++) if (m->up[i] == serial) return 1;
    return 0;
}
static inline uint64_t mvbp_t(const mvbp *m, uint64_t frame) { return m->frame == frame ? m->t : 0; }
static inline int mvbp_t_tid(const mvbp *m) { return m->t_tid; }   // T of the last conversion
static inline int mvbp_ready(const mvbp *m) { return m->ready; }

static inline int mvbp_step(mvbp *m, uint64_t frame, const mv_disp *d) {
    if (d->reads_upload && d->image_serial) {   // the conversion: a movie frame starts
        m->frame = frame; m->t = d->image_serial; m->t_tid = d->image_tid; m->t_w = d->img_w; m->t_h = d->img_h; m->idx = m->ncand = m->overflow = 0;
        return MV_RUN;
    }
    if (m->frame != frame) return MV_NONE;
    if (d->image_serial && d->image_serial != m->t && d->img_w > m->t_w && d->img_h > m->t_h) {   // the upscale: the frame's chain ends
        m->frame = UINT64_MAX;
        if (m->ready && m->idx != m->nchain) m->mismatched++;
        return MV_UPSCALE;
    }
    if (m->ready) {
        if (m->idx < m->nchain && !strncmp(d->key, m->chain[m->idx], 16)) { m->idx++; m->skipped++; return MV_SKIP; }
        return MV_RUN;
    }
    if (m->ncand < MV_CHAIN) { strncpy(m->cand[m->ncand], d->key, 16); m->cand[m->ncand][16] = 0; m->ncand++; } else m->overflow = 1;
    return MV_RUN;
}
/* The caller has a scaler ready at the upscale of a frame that ran the network: skip its sequence from the next frame. */
static inline void mvbp_learned(mvbp *m) {
    if (m->overflow) return;
    memcpy(m->chain, m->cand, sizeof m->chain); m->nchain = m->ncand; m->ready = 1;
}
static inline void mvbp_failed(mvbp *m) { m->ready = 0; }
static inline void mvbp_off(mvbp *m) { m->ready = 0; }
