/* probe-gpu-arena-trim (included after the production backend): a burst of argument blocks in one submission (as while
   loading) is released after STOR_IDLE_SUBMITS idle submissions and leaves the residency set, while the block that every
   later submission reuses stays resident. Negative control OOT_ARENA_TRIM_PROBE_NEGATIVE=1 skips the idle window. */
int mtl_probe_arena_trim(void) {
    mtl_init(); int checked = 0; op_hdr end = {OP_END, 8};
    int negative = getenv("OOT_ARENA_TRIM_PROBE_NEGATIVE") ? atoi(getenv("OOT_ARENA_TRIM_PROBE_NEGATIVE")) : 0;
    enum { BURST = 20 }; const size_t block = 4u << 20;
    uint64_t released0 = g_arenas_released;
    mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
    __unsafe_unretained id<MTLBuffer> b = nil; NSMutableArray *burst = [NSMutableArray array];
    for (int i = 0; i < BURST; i++) { arena_alloc_u(block, &b, NULL, NULL); [burst addObject:b]; }
    mtl_sub_commit(); mtl_finish();
    pthread_mutex_lock(&g_res_mu); int resident = 0; for (id x in burst) resident += [g_res_objects containsObject:x]; pthread_mutex_unlock(&g_res_mu);
    if (resident != BURST) MDIE("arena trim probe: %d of %d burst blocks resident", resident, BURST);
    checked++;
    id<MTLBuffer> kept = nil;
    int rounds = negative == 1 ? 8 : STOR_IDLE_SUBMITS + 8;
    for (int r = 0; r < rounds; r++) {
        mtl_sub_begin(); mtl_exec((const uint8_t *)&end);
        arena_alloc_u(256, &b, NULL, NULL); kept = b;   // the working set: one block reused every submission
        mtl_sub_commit(); mtl_finish();   // one submission in flight: the working set is one or two blocks
    }
    mtl_finish(); mtl_sub_begin(); mtl_exec((const uint8_t *)&end); mtl_sub_commit(); mtl_finish();
    uint64_t released = g_arenas_released - released0;
    if (released < BURST - 2) MDIE("arena trim probe: %llu of %d idle burst blocks released", (unsigned long long)released, BURST);
    checked++;
    pthread_mutex_lock(&g_res_mu); resident = 0; for (id x in burst) resident += [g_res_objects containsObject:x];
    int kept_res = [g_res_objects containsObject:kept]; pthread_mutex_unlock(&g_res_mu);
    if (resident > 2) MDIE("arena trim probe: %d burst blocks still resident, the working set is at most 2", resident);
    if (!kept_res) MDIE("arena trim probe: the block in use left the residency set");
    checked += 2;
    MLOG("arena trim probe: %llu idle blocks released, working block kept", (unsigned long long)released);
    return checked;
}
