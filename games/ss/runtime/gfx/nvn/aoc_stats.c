#include "aoc_stats.h"
#include <pthread.h>
#include <stdlib.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int enabled;
static AocStats latest;
static double intervals[AOC_STATS_SAMPLES], shown[AOC_STATS_SAMPLES];
static unsigned count, shown_count;
static uint64_t previous_frame, previous_display_callback;
static double previous_display;
int aoc_stats_enabled(void) { return atomic_load_explicit(&enabled,memory_order_relaxed); }
void aoc_stats_enable(int on) {
    pthread_mutex_lock(&mutex);
    if (!!on != aoc_stats_enabled()) {
        memset(&latest,0,sizeof latest);
        count=shown_count=0;
        previous_frame=previous_display_callback=0; previous_display=0;
        atomic_store_explicit(&enabled,!!on,memory_order_relaxed);
    }
    pthread_mutex_unlock(&mutex);
}
static void push(double *p, unsigned *n, double value) {
    if (*n < AOC_STATS_SAMPLES) p[(*n)++] = value;
    else { for (unsigned i=1;i<AOC_STATS_SAMPLES;i++) p[i-1]=p[i]; p[AOC_STATS_SAMPLES-1]=value; }
}
static double fps(const double *p, unsigned n) {
    double total=0; unsigned used=0;
    while (used<n && total<1000) total+=p[n-1-used++];
    return total>0 ? used*1000.0/total : 0;
}
void aoc_stats_frame(uint64_t now, double gpu, int target) {
    if (!aoc_stats_enabled()) return;
    pthread_mutex_lock(&mutex);
    if (!aoc_stats_enabled()) { pthread_mutex_unlock(&mutex); return; }
    if (previous_frame && now>previous_frame) push(intervals,&count,(now-previous_frame)/1e6);
    previous_frame=now; latest.gpu_ms=gpu; latest.target=target;
    pthread_mutex_unlock(&mutex);
}
void aoc_stats_display(double seconds, uint64_t now) {
    if (!aoc_stats_enabled() || !(seconds>0) || !isfinite(seconds)) return;
    pthread_mutex_lock(&mutex);
    if (!aoc_stats_enabled()) { pthread_mutex_unlock(&mutex); return; }
    if (seconds>previous_display && previous_display>0) push(shown,&shown_count,(seconds-previous_display)*1000);
    if (seconds>previous_display) { previous_display=seconds; previous_display_callback=now; }
    pthread_mutex_unlock(&mutex);
}
void aoc_stats_resources(AocShaderStats shaders) {
    if (!aoc_stats_enabled()) return;
    pthread_mutex_lock(&mutex);
    if (aoc_stats_enabled()) latest.shaders=shaders;
    pthread_mutex_unlock(&mutex);
}
static int compare(const void *a,const void *b) { double x=*(const double *)a,y=*(const double *)b; return (x>y)-(x<y); }
static double percentile(const double *p,unsigned n,double fraction) {
    if (!n) return 0;
    double index=(n-1)*fraction; unsigned lo=(unsigned)index,hi=lo+1<n?lo+1:lo;
    return p[lo]+(p[hi]-p[lo])*(index-lo);
}
AocStats aoc_stats_snapshot(uint64_t now) {
    double sorted[AOC_STATS_SAMPLES];
    pthread_mutex_lock(&mutex);
    AocStats s=latest; s.samples=count;
    for (unsigned i=0;i<count;i++) s.history[i]=sorted[i]=intervals[i];
    s.active=previous_frame && now>=previous_frame && now-previous_frame<1000000000ull;
    s.display_active=previous_display_callback && now>=previous_display_callback && now-previous_display_callback<1000000000ull;
    s.game_fps=s.active?fps(intervals,count):0; s.display_fps=s.display_active?fps(shown,shown_count):0;
    pthread_mutex_unlock(&mutex);
    qsort(sorted,count,sizeof(double),compare);
    s.frame_ms=count?s.history[count-1]:0;
    s.p50_ms=percentile(sorted,count,.5); s.p95_ms=percentile(sorted,count,.95); s.p99_ms=percentile(sorted,count,.99);
    return s;
}
