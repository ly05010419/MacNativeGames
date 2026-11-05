// TOTK: which pipeline archive file holds each key (par_load_dir), and a key whose stored pipeline no longer loads
// (par_repair). Included by totk_pipeline_archive.h after its globals and file names; MLOG comes from its includer.
#pragma once
/* The keys of the files in the folder (g_par_have, g_par_where); removes what a write cut short left, unless another
   process is publishing (totk_pipeline_archive_publish.h par_pub_scan). Returns the files' bytes. A key in two files (a
   repaired one, par_repair) is the newest file's: the later modification time of its key list, then the larger name,
   so it does not depend on the order the folder is listed in. */
static uint64_t par_load_dir(NSUInteger *files) {
    NSFileManager *fm = NSFileManager.defaultManager;
    g_par_have = [NSMutableSet set]; g_par_where = [NSMutableDictionary dictionary]; g_par_pending = [NSMutableSet set];
    g_par_open = [NSMutableDictionary dictionary]; uint64_t bytes = 0; *files = 0;
    for (NSString *base in par_pub_scan(par_dir(), par_lock_file(), YES, [NSSet setWithObject:@"identity.json"])) {   // oldest first
        NSString *list = [NSString stringWithContentsOfFile:par_keys_file(base) encoding:NSUTF8StringEncoding error:nil];
        for (NSString *key in [list componentsSeparatedByString:@"\n"]) if (key.length) { [g_par_have addObject:key]; g_par_where[key] = base; }
        bytes += [[fm attributesOfItemAtPath:par_file(base) error:nil] fileSize]; ++*files;
    }
    return bytes;
}
/* A stored pipeline that Metal no longer finds in its file (MTLPipelineOptionFailOnBinaryArchiveMiss), compiled without
   it: the key is the pipeline state, not the code, so a host change to the MSL or the descriptor without a new
   PAR_SCHEMA or chain keeps the key and changes the pipeline. Until 2026-11-04 par_write skipped the key (in g_par_have)
   and such a pipeline was compiled in every session. Now pso_new / cs_new drop the key here once the pipeline compiled,
   so the warm-up's and fill's threads write it again (par_write) into a new file, which wins at the next start
   (par_load_dir); one met in play is in pso_user.tsv for the next warm-up. Dropped only while the key still names the file
   that missed (another thread may have repaired it meanwhile) and once per key a session (two pipelines that share a key
   would rewrite each other). A defence only: a change of the translation still needs a new PAR_SCHEMA or chain. */
static NSMutableSet<NSString *> *g_par_repaired;   // keys dropped this session (g_par_mu)
static _Atomic uint64_t g_par_repairs;
static int par_repair(NSString *key, NSString *from) {
    if (!key || !from) return 0;
    pthread_mutex_lock(&g_par_mu);
    if (!g_par_repaired) g_par_repaired = [NSMutableSet set];
    int drop = [g_par_where[key] isEqualToString:from] && ![g_par_repaired containsObject:key];
    if (drop) { [g_par_have removeObject:key]; [g_par_where removeObjectForKey:key]; [g_par_repaired addObject:key]; }
    pthread_mutex_unlock(&g_par_mu);
    if (drop) {
        uint64_t n = atomic_fetch_add(&g_par_repairs, 1) + 1;
        if (n <= 4 || n % 500 == 0) MLOG("[pipeline-archive] a stored pipeline no longer loads from %s: compiled; its key is dropped, to be written again by the warm-up or the fill (%llu this run)", from.UTF8String, (unsigned long long)n);
    }
    return drop;
}
