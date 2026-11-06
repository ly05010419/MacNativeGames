/* Regression: the present thread advances while a submission and its delayed
   feedback still belong to the earlier frame. The global-frame negative
   control must fail the same ownership assertions. */
#include "../third_party/d2r_gfx/gfx/nvn/totk_timeline.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void event(int kind, uint64_t t, uint64_t a, uint64_t b) {
    if (getenv("OOT_TIMELINE_GLOBAL_NEGATIVE")) totk_tl_at(kind, t, a, b);
    else totk_tl_at_frame(kind, t, 1, a, b);
}
static void *feedback(void *unused) {
    (void)unused; event(TL_GPU, 200, 7, 300); return NULL;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char config[4096];
    if (snprintf(config,sizeof config,"%s:1:1",argv[1]) >= (int)sizeof config) return 2;
    if (setenv("TOTK_TIMELINE",config,1)) return 2;
    totk_tl_frame(1);
    totk_tl_frame(2);
    event(TL_COMMIT,100,7,1); event(TL_FENCE,101,7,77);
    totk_tl_frame(3);
    pthread_t worker;
    if (pthread_create(&worker,NULL,feedback,NULL) || pthread_join(worker,NULL)) return 2;
    totk_tl_at(TL_SUB_IN,400,99,0); /* Unowned events still use the current frame. */
    totk_tl_frame(32);
    FILE *f=fopen(argv[1],"r"); if (!f) return 2;
    char line[512],kind[32]; unsigned long long t,frame,a,b;unsigned tid;int checked=0;
    while (fgets(line,sizeof line,f)) {
        if (line[0]=='#') continue;
        if (sscanf(line,"%llu %31s %u %llu %llu %llu",&t,kind,&tid,&frame,&a,&b)!=6) return 2;
        uint64_t expected=!strcmp(kind,"sub_in")?3:1;
        if (frame!=expected) { fprintf(stderr,"timeline owner mismatch: %s frame%llu expected%llu\n",kind,frame,(unsigned long long)expected);fclose(f);return 1; }
        checked++;
    }
    fclose(f);if (checked!=4) return 1;
    printf("{\"status\":\"pass\",\"probe\":\"timeline-owner\",\"checked\":%d}\n",checked);return 0;
}
