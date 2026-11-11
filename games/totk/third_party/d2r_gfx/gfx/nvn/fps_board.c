#include "fps_board.h"
#include <math.h>
#include <pthread.h>
#include <string.h>

/* Frame times to the nearest 0.1 ms, up to the gap (longer ones are not counted). */
#define BUCKET_S 0.0001
#define BUCKETS 5001   /* FPS_BOARD_GAP_S / BUCKET_S + 1 */
_Static_assert(BUCKETS * BUCKET_S >= FPS_BOARD_GAP_S, "the buckets reach the gap");

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int counting, have_last;
    uint64_t last_k;
    double last_t;
    uint32_t hist[BUCKETS];
    uint64_t frames, gen;
    double seconds;
    double win_t0, best;   /* the open one-second window (win_t0 < 0: none) and the best rate of a closed one */
    uint64_t win_n;
} g = {.win_t0 = -1};

static void forget(void) { memset(&g, 0, sizeof g); g.win_t0 = -1; }

void fps_board_reset(void) { pthread_mutex_lock(&g_mu); forget(); pthread_mutex_unlock(&g_mu); }
void fps_board_start(void) { pthread_mutex_lock(&g_mu); g.counting = 1; pthread_mutex_unlock(&g_mu); }
int fps_board_counting(void) { pthread_mutex_lock(&g_mu); int c = g.counting; pthread_mutex_unlock(&g_mu); return c; }

/* n frames over dt seconds, each dt / n long. */
static void count(uint64_t n, double dt, double now_s) {
    double ft = dt / (double)n;
    if (ft > FPS_BOARD_GAP_S) { g.win_t0 = now_s; g.win_n = 0; return; }   // a pause: not play, and no window across it
    int b = (int)floor(ft / BUCKET_S + 0.5);
    g.hist[b < BUCKETS ? b : BUCKETS - 1] += (uint32_t)n;
    g.frames += n; g.seconds += dt;
    if (g.win_t0 < 0) g.win_t0 = now_s - dt;
    g.win_n += n;
    if (now_s - g.win_t0 >= 1.0) {
        double rate = (double)g.win_n / (now_s - g.win_t0);
        if (rate > g.best) g.best = rate;
        g.win_t0 = now_s; g.win_n = 0;
    }
}

void fps_board_note(uint64_t ticket, double now_s) {
    pthread_mutex_lock(&g_mu);
    if (g.have_last && ticket > g.last_k && now_s > g.last_t && g.counting) count(ticket - g.last_k, now_s - g.last_t, now_s);
    g.have_last = 1; g.last_k = ticket; g.last_t = now_s;
    pthread_mutex_unlock(&g_mu);
}

void fps_board_note_gen(double now_s) {
    (void)now_s;
    pthread_mutex_lock(&g_mu);
    if (g.counting) g.gen++;
    pthread_mutex_unlock(&g_mu);
}

static double bucket_s(int b) { return (b > 0 ? b : 0.5) * BUCKET_S; }

int fps_board_snapshot(fps_board_stats *out) {
    pthread_mutex_lock(&g_mu);
    int got = g.frames > 0 && g.seconds > 0;
    if (got) {
        fps_board_stats s = {.seconds = g.seconds, .frames = g.frames};
        s.avg = (double)g.frames / g.seconds;
        uint64_t half = (g.frames + 1) / 2, seen = 0;
        for (int b = 0; b < BUCKETS; b++) { seen += g.hist[b]; if (seen >= half) { s.median = 1.0 / bucket_s(b); break; } }
        uint64_t want = g.frames / 100 ? g.frames / 100 : 1, left = want;
        double slow = 0;
        for (int b = BUCKETS - 1; b >= 0 && left; b--) {
            uint64_t take = g.hist[b] < left ? g.hist[b] : left;
            slow += (double)take * bucket_s(b); left -= take;
        }
        s.low1 = (double)want / slow;
        s.max = fmax(g.best, fmax(s.median, s.avg));
        s.interp_fps = (double)g.gen / g.seconds;
        *out = s;
    }
    pthread_mutex_unlock(&g_mu);
    return got;
}
