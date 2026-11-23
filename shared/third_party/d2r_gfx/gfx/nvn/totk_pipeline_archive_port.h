/* The ports built with Tears of the Kingdom's graphics layer (Animal Crossing, Super Mario Odyssey) make their pipelines
   with Tears of the Kingdom's pipeline archive (totk_pipeline_archive.h, shared; user 2026-11-21: "统一 … 用测试保证"):
   a pipeline is first asked of the file holding its key, with MTLPipelineOptionFailOnBinaryArchiveMiss. Found: done.
   Not found: compiled as usual; a key whose file did not hold it is forgotten (par_stale), and the warm-up's thread
   writes it into a new file (par_add: par_write_in_place), as Tears of the Kingdom's pso_new / cs_new_ do. Files the Mac
   compiled for the device's GPU while packaging (pipelines-<arch>-* in the seed, device_pipelines.py) are found the
   same way (par_load_prebuilt). Before (2026-10-22, from Odyssey upstream) the ports had a copy of an older archive:
   one file per key, written when the game was idle, and no files from the Mac.
   Included by the port's nvn_mtl.m in place of totk_pipeline_archive.h, after pso_skey_from, MLOG and g_dev; the
   translation chain is the port's (its own dmsh_chain.h, found beside this file's link). Its nvn_mtl.m keeps the
   warm-up (warm_start: with a valid archive no shader library is loaded for itself) and the packaging mode
   (totk_pipeline_archive_port_fill.h). TOTK_PIPELINE_ARCHIVE=0 turns it off. */
#pragma once
#include "totk_pipeline_archive.h"
static _Atomic uint64_t g_par_hits, g_par_misses;
static void par_count(int hit) {
    uint64_t h = hit ? atomic_fetch_add(&g_par_hits, 1) + 1 : atomic_load(&g_par_hits), m = hit ? atomic_load(&g_par_misses) : atomic_fetch_add(&g_par_misses, 1) + 1;
    if ((h + m) % 1000 == 0) MLOG("管线存档：建管线 %llu 次，从存档取 %llu 次，现编 %llu 次", (unsigned long long)(h + m), (unsigned long long)h, (unsigned long long)m);
}
/* As Tears of the Kingdom's pso_archive_key without a variant: a border or tile-read pipeline (its fragment function is a
   variant of the shader's) shares the plain pipeline's state, so it would share its key and miss the plain one's file in
   every session (totk_pipeline_archive.h PAR_SCHEMA 5); it is not archived. */
static NSString *pso_archive_key(const pso_key *k, NvShader *vs, NvShader *fs, int merged) {
    if (!merged && ((k->a2c & PSO_VBORDER) || k->fsfn)) return nil;
    pso_skey sk; pso_skey_from(k, vs, fs, merged, NULL, &sk); return par_hash(&sk, sizeof sk);
}
static NSString *cs_archive_key(NvShader *cs) { return cs ? [@"c" stringByAppendingString:par_hash(cs->key, strnlen(cs->key, sizeof cs->key))] : nil; }
static id<MTLRenderPipelineState> pso_new_(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e);
static id<MTLRenderPipelineState> pso_new(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e) {
    DM_SP_BEGIN(sp, "pipeline-render", "%{public}s", key ? key.UTF8String : "-");
    id<MTLRenderPipelineState> ps = pso_new_(d, key, e);
    DM_SP_END(sp, "pipeline-render");
    return ps;
}
static id<MTLRenderPipelineState> pso_new_(MTLRenderPipelineDescriptor *d, NSString *key, NSError **e) {
    id<MTLRenderPipelineState> ps = nil;
    id<MTLBinaryArchive> a = par_archive_for(g_dev, key);
    if (a) {
        d.binaryArchives = @[a];
        ps = [g_dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        par_pre_note(ps != nil);
        if (ps) { par_count(1); return ps; }
        par_stale(key);
    }
    if (key && g_par_on) par_count(0);
    ps = [g_dev newRenderPipelineStateWithDescriptor:d error:e];
    if (ps) par_add(g_dev, key, d);
    return ps;
}
static id<MTLComputePipelineState> cs_new_(NvShader *cs, NSError **e);
static id<MTLComputePipelineState> cs_new(NvShader *cs, NSError **e) {
    DM_SP_BEGIN(sp, "pipeline-compute", "%{public}s", cs ? cs->key : "-");
    id<MTLComputePipelineState> ps = cs_new_(cs, e);
    DM_SP_END(sp, "pipeline-compute");
    return ps;
}
static id<MTLComputePipelineState> cs_new_(NvShader *cs, NSError **e) {
    if (!cs || !cs->fn) return nil;
    NSString *key = cs_archive_key(cs);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = cs->fn;
    id<MTLComputePipelineState> ps = nil;
    id<MTLBinaryArchive> a = par_archive_for(g_dev, key);
    if (a) {
        d.binaryArchives = @[a];
        ps = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        d.binaryArchives = nil;
        par_pre_note(ps != nil);
        if (ps) { par_count(1); return ps; }
        par_stale(key);
    }
    if (g_par_on) par_count(0);
    ps = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:e];
    if (ps) par_add(g_dev, key, d);
    return ps;
}
