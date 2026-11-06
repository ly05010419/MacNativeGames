/* Host descriptor preparation diagnostics. No shader or resource contents are
 * changed. OOT_DESCRIPTOR_PROFILE_FILE plus `flag descriptor_profile 1` opts in;
 * these intervals are never normal performance measurements. */
#ifndef NVN_DESCRIPTOR_PROFILE_H
#define NVN_DESCRIPTOR_PROFILE_H
enum { DP_SERIAL, DP_ALL, DP_STORAGE, DP_STORAGE_EMPTY, DP_STORAGE_REMOVED,
       DP_POOL, DP_SCANNED, DP_RETRY, DP_ROWS_DROPPED, DP_COUNT };
static const char *const dp_names[DP_COUNT] = {
    "serial_steps", "global_steps", "storage_releases", "empty_storage_releases",
    "removed_holders", "pool_attempts", "scanned_entries", "pool_retries", "rows_dropped"
};
static _Atomic int g_dp_active;
static _Atomic uint64_t g_dp_counts[DP_COUNT];
static pthread_mutex_t g_dp_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_dp_file;
static uint64_t g_dp_rows;
static inline int descriptor_profile_on(void) { return atomic_load_explicit(&g_dp_active, memory_order_relaxed); }
static inline void dp_add(int counter, uint64_t value) {
    if (descriptor_profile_on()) atomic_fetch_add_explicit(&g_dp_counts[counter], value, memory_order_relaxed);
}
static int dp_row_locked(void) {
    if (!g_dp_file || !descriptor_profile_on()) return 0;
    if (g_dp_rows++ < 65536) return 1;
    atomic_fetch_add_explicit(&g_dp_counts[DP_ROWS_DROPPED], 1, memory_order_relaxed); return 0;
}
static void descriptor_profile_set(int on) {
    pthread_mutex_lock(&g_dp_mu);
    if (on && !descriptor_profile_on()) {
        if (!g_dp_file) {
            const char *path = getenv("OOT_DESCRIPTOR_PROFILE_FILE");
            if (path && *path) g_dp_file = fopen(path, "w");
            if (!g_dp_file) { pthread_mutex_unlock(&g_dp_mu); MLOG("descriptor profile needs a writable OOT_DESCRIPTOR_PROFILE_FILE"); return; }
            fprintf(g_dp_file, "#P\tframe\tfence\tslot\tkind\tbase\tn\tser_old\tser_start\tser_end\trev_old\trev_start\trev_end\tdirty_old\tdirty_start\tsub_old\tsub_now\tmemory_old\tmemory_now\treasons\tfull\tchecked\tcpu_refresh\tnew_buffer\tretry\tns\n");
            fprintf(g_dp_file, "#S\tframe\tfence\twhy\tentry\tmem\tsize\tw\th\td\tlevels\ttarget\ttexture\tholders\tremoved\tser_gen\tserials\n");
        }
        for (int i=0; i<DP_COUNT; i++) atomic_store_explicit(&g_dp_counts[i], 0, memory_order_relaxed);
        g_dp_rows=0; fprintf(g_dp_file, "B\t%llu\t%llu\n", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_fence);
        atomic_store_explicit(&g_dp_active, 1, memory_order_release);
    } else if (!on && descriptor_profile_on()) {
        atomic_store_explicit(&g_dp_active, 0, memory_order_release);
        for (int i=0; i<DP_COUNT; i++) fprintf(g_dp_file, "C\t%s\t%llu\n", dp_names[i], (unsigned long long)atomic_load_explicit(&g_dp_counts[i], memory_order_relaxed));
        fprintf(g_dp_file, "E\t%llu\t%llu\n", (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_fence); fflush(g_dp_file);
    }
    pthread_mutex_unlock(&g_dp_mu);
    MLOG("[flag] descriptor_profile = %d (diagnostic-only)", descriptor_profile_on());
}
typedef struct {
    uint64_t start, ser_old, ser_start, rev_old, rev_start, dirty_old, dirty_start, sub_old, memory_old, memory_now;
    int slot, kind, base, n, reasons, cpu_refresh;
} dp_pool_event;
/* reasons: 1 replacement, 2 descriptor journal overflow, 4 serial journal
 * overflow, 8 pool lifetime, 16 an all-serial step encountered in the journal.
 * retry: 1 pass split, 2 global serial change while resolving the pool. */
static void descriptor_profile_pool(const dp_pool_event *p, int full, uint64_t checked, int buffer, int retry,
                                    uint64_t ser_end, uint64_t rev_end, uint64_t sub_now) {
    if (!p->start) return;
    uint64_t elapsed=bp_now()-p->start;
    dp_add(DP_POOL,1);dp_add(DP_SCANNED,checked);if(retry)dp_add(DP_RETRY,1);
    pthread_mutex_lock(&g_dp_mu);
    if (dp_row_locked()) fprintf(g_dp_file,
        "P\t%llu\t%llu\t%d\t%d\t%d\t%d\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%llu\t%d\t%d\t%llu\t%d\t%d\t%d\t%llu\n",
        (unsigned long long)atomic_load(&g_cur_frame),(unsigned long long)g_fence,p->slot,p->kind,p->base,p->n,
        (unsigned long long)p->ser_old,(unsigned long long)p->ser_start,(unsigned long long)ser_end,
        (unsigned long long)p->rev_old,(unsigned long long)p->rev_start,(unsigned long long)rev_end,
        (unsigned long long)p->dirty_old,(unsigned long long)p->dirty_start,(unsigned long long)p->sub_old,(unsigned long long)sub_now,
        (unsigned long long)p->memory_old,(unsigned long long)p->memory_now,p->reasons,full,(unsigned long long)checked,p->cpu_refresh,buffer,retry,(unsigned long long)elapsed);
    pthread_mutex_unlock(&g_dp_mu);
}
static void descriptor_profile_storage(const char *why, const void *entry, uint64_t mem, uint64_t size,
        unsigned w,unsigned h,unsigned d,unsigned levels,unsigned target,const void *texture,
        const uint64_t *serials,int holders,int removed,uint64_t ser_gen) {
    if (!descriptor_profile_on()) return;
    dp_add(DP_STORAGE,1);dp_add(DP_STORAGE_EMPTY,holders==0);dp_add(DP_STORAGE_REMOVED,(uint64_t)removed);
    pthread_mutex_lock(&g_dp_mu);
    if (dp_row_locked()) {
        fprintf(g_dp_file,"S\t%llu\t%llu\t%s\t%p\t%#llx\t%llu\t%u\t%u\t%u\t%u\t%u\t%p\t%d\t%d\t%llu\t",
            (unsigned long long)atomic_load(&g_cur_frame),(unsigned long long)g_fence,why,entry,(unsigned long long)mem,(unsigned long long)size,
            w,h,d,levels,target,texture,holders,removed,(unsigned long long)ser_gen);
        for (int i=0;i<holders;i++)fprintf(g_dp_file,"%s%llu",i?",":"",(unsigned long long)serials[i]);
        fputc('\n',g_dp_file);
    }
    pthread_mutex_unlock(&g_dp_mu);
}
#endif
