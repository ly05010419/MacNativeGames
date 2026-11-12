/* NVN2 task/mesh: original control metadata -> native Metal object/mesh stages.
 * API/layout reference: ChiefSZ13/nxnative ee9b7765, MIT. Pipeline creation is
 * synchronous here: every requested draw must execute with its complete stages. */
static struct { int on,profile_grid; NvShader *ts,*ms,*fs; uint32_t first,count; uint64_t first_gpu,indirect_gpu; } g_mtk;
static int mtk_owners(NvShader **ts,NvShader **ms){if(!g_mtk.on)return 0;*ts=g_mtk.ts;*ms=g_mtk.ms;return 1;}   // nvn_mesh_batch.inc: the mesh draw's real task / mesh owners (draw_inner sees a raster-only program)
typedef struct { pso_key p; void *task; uint64_t variant; } mtk_pso_key;
static nvn_hmap g_mtk_cache={.klen=sizeof(mtk_pso_key)};
#include "nvn_mesh_capture.inc"
#include "nvn_mesh_translation.inc"
#include "nvn_mesh_pack.inc"
/* Diagnostic only: copy the selected draw's storage inputs on the same GPU
 * timeline, after their producers and before this draw. A CPU memcpy while
 * encoding observes older contents when an earlier dispatch has not run yet.
 * Capture each selected fragment program once; normal rendering is unchanged.
 * OOT_MESH_INPUT_CAPTURE=<external directory>, OOT_MESH_INPUT_CAPTURE_FS=<prefix,...>.
 */
static void mtk_capture_storage(NvProg *pr) {
    static int initialized;static NSString *directory;static const char *prefixes;static NSMutableDictionary<NSString *,NSNumber *> *done;
    if(!initialized){initialized=1;const char *path=getenv("OOT_MESH_INPUT_CAPTURE");prefixes=getenv("OOT_MESH_INPUT_CAPTURE_FS");
        if(path&&*path&&prefixes&&*prefixes){directory=@(path);done=[NSMutableDictionary dictionary];}}
    NvShader *fs=pr->sh[1];if(!directory||!fs)return;
    NSString *key=@(fs->key);uint64_t frame=atomic_load(&g_cur_frame);NSNumber *previous=done[key];
    if(!diag_shader_prefix_csv_matches(fs->key,prefixes)||(previous&&(!passdump()||previous.unsignedLongLongValue==frame)))return;done[key]=@(frame);
    NSString *stem=[NSString stringWithFormat:@"%@-F%llu",key,frame];
    NSMutableArray *records=[NSMutableArray array];
    for(int st=1;st<5;st++){
        NvShader *sh=pr->sh[st];if(!sh)continue;upd_guard_stage(sh,st);
        unsigned captured=0;
        for(int i=0;i<sh->rf.nb;i++){
            const nvn_rbind *b=&sh->rf.b[i];int reg=b->reg;
            if(b->kind!=NVN_RB_SBUFP||reg<0||reg>=16||(captured&(1u<<reg)))continue;captured|=1u<<reg;
            const p_bind_buf *u=&S.ssbo[st][reg];if(!u->addr||!u->size)continue;
            size_t size=(size_t)MIN(u->size,UINT64_C(64)<<20);void *source;uint64_t offset,gpu;
            if(!mtl_gpu_addr(u->addr,size,&source,&offset,&gpu)){MLOG("[mesh-input] unmapped stage %d storage %d",st,reg);continue;}
            id<MTLBuffer> capture=[g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
            if(!capture){MLOG("[mesh-input] cannot allocate %zu bytes",size);continue;}res_add(capture);
            enc_b_copy_buffer(blit_buffer((__bridge id<MTLBuffer>)source,offset,capture,0,size),(__bridge id<MTLBuffer>)source,offset,capture,0,size);
            NSString *name=[NSString stringWithFormat:@"%@-st%d-sb%d.bin",stem,st,reg];NSString *path=[directory stringByAppendingPathComponent:name];
            [records addObject:@{@"shader":@(sh->key),@"stage":@(st),@"register":@(reg),@"address":[NSString stringWithFormat:@"%#llx",u->addr],
                @"bound_size":@(u->size),@"captured_size":@(size),@"file":name}];
            [g_commit_blocks addObject:^{BOOL ok=[[NSData dataWithBytes:capture.contents length:size]writeToFile:path atomically:YES];
                MLOG("[mesh-input] GPU snapshot %s: %s (%zu bytes)",ok?"saved":"FAILED",path.UTF8String,size);}];
            defer_obj_release(capture);
        }
    }
    NSDictionary *manifest=@{@"fragment":key,@"frame":@(atomic_load(&g_cur_frame)),@"first_task":@(g_mtk.first),@"task_count":@(g_mtk.count),
        @"timing":@"GPU copies before selected draw; files written after submission completion",@"storage":records};
    NSString *path=[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".json"]];
    [g_commit_blocks addObject:^{NSData *data=[NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil];
        if(![data writeToFile:path atomically:YES])MLOG("[mesh-input] FAILED manifest %s",path.UTF8String);}];
}
/* Match the actual mesh vertex/primitive outputs to fragment inputs. Unwritten
 * varyings follow the existing raster path's zero initialization, but a mesh
 * function must be extended through dm_vertex_t and set_vertex, never main0_out. */
static id<MTLFunction> mtk_mesh_fn_base(NvShader *ms,NvShader *fs,id<MTLFunction> base) {
    if(!fs||!sh_has_src(fs))return base;
    os_unfair_lock_lock(&g_vpatch_lock); // io is shared with background raster pipeline compilation
    NSDictionary *inputs=msl_varyings(fs),*outputs=msl_varyings(ms);
    NSMutableString *recipe=[NSMutableString stringWithFormat:@"mesh;%p;",(__bridge void*)base];
    NSMutableString *fields=[NSMutableString string];
    for(NSNumber *location in [inputs.allKeys sortedArrayUsingSelector:@selector(compare:)])if(!outputs[location]) {
        [recipe appendFormat:@"%@:%@;",location,inputs[location]];
        [fields appendFormat:@"    %@ oot_unwritten_%@ [[user(locn%@)]];\n",inputs[location],location,location];
    }
    if(!fields.length){os_unfair_lock_unlock(&g_vpatch_lock);return base;}
    id<MTLFunction> function=ms->vpatch[recipe];os_unfair_lock_unlock(&g_vpatch_lock);
    if(function)return function;
    NSString *source=fn_src(base)?:sh_src(ms),*head=@"struct dm_vertex_t {";
    NSRange declaration=[source rangeOfString:head];
    if(declaration.location==NSNotFound||[source rangeOfString:@"dm_vertex_t v;"].location==NSNotFound)MDIE("mesh shader %s has no vertex output declaration",ms->key);
    source=[source stringByReplacingCharactersInRange:declaration withString:[head stringByAppendingFormat:@"\n%@",fields]];
    source=[source stringByReplacingOccurrencesOfString:@"dm_vertex_t v;" withString:@"dm_vertex_t v = {};"];
    NSError *error=nil;id<MTLLibrary> library=variant_library(source,&error);
    function=library?[library newFunctionWithName:ms->fn.name]:nil;
    if(!function)MDIE("mesh shader %s output variant: %s",ms->key,error.localizedDescription.UTF8String);
    fn_note_src(function,source);
    os_unfair_lock_lock(&g_vpatch_lock);
    if(!ms->vpatch)ms->vpatch=[NSMutableDictionary dictionary];
    if(!ms->vpatch[recipe])ms->vpatch[recipe]=function;function=ms->vpatch[recipe];
    os_unfair_lock_unlock(&g_vpatch_lock);return function;
}
static id<MTLFunction> mtk_mesh_fn(NvShader *ms,NvShader *fs) { return mtk_mesh_fn_base(ms,fs,ms->fn); }
static int mtk_empty_export_on(void) {
    int v=atomic_load_explicit(&g_mesh_empty_export,memory_order_relaxed);
    if(v<0){const char *e=getenv("OOT_MESH_EMPTY_EXPORT");v=e?atoi(e)!=0:0;atomic_store(&g_mesh_empty_export,v);}
    return v;
}
/* The generated wrapper has finished the complete guest body and its final
   threadgroup barrier before publishing np. No rasterizer consumes vertices
   when np is zero. Keep every guest memory side effect, and skip only the host
   wrapper's now-unused vertex exports. Source must match our own MeshStage ABI;
   arbitrary mesh functions keep the original path. */
static id<MTLFunction> mtk_empty_export_fn(id<MTLFunction> base,NvShader *ms) {
    if(!mtk_empty_export_on())return base;
    static NSMutableDictionary<NSValue *,id<MTLFunction>> *cache;
    if(!cache)cache=[NSMutableDictionary dictionary];NSValue *key=[NSValue valueWithPointer:(__bridge void *)base];
    if(cache[key])return cache[key];
    NSString *source=fn_src(base)?:sh_src(ms);
    NSRegularExpression *end=[NSRegularExpression regularExpressionWithPattern:@"if\\([a-zA-Z0-9_.]+==0u\\)dm_out\\.set_primitive_count\\(np\\);\\n" options:0 error:nil];
    if(!source||![source containsString:@"dm_stage_body("]||![source containsString:@"uint np=min("]||
       ![end numberOfMatchesInString:source options:0 range:NSMakeRange(0,source.length)]){cache[key]=base;return base;}
    NSString *lid=nil;{NSRegularExpression *rel=[NSRegularExpression regularExpressionWithPattern:@"dm_release_slot\\(dm_scratch,dm_slot_tg,(\\w+)\\);" options:0 error:nil];
        NSTextCheckingResult *r=[rel firstMatchInString:source options:0 range:NSMakeRange(0,source.length)];if(r)lid=[source substringWithRange:[r rangeAtIndex:1]];}
    NSString *tpl=lid?[NSString stringWithFormat:@"$0if(np==0u){dm_release_slot(dm_scratch,dm_slot_tg,%@);return;}\n",lid]:@"$0if(np==0u)return;\n";   // device stage memory: give the slot back before leaving
    source=[end stringByReplacingMatchesInString:source options:0 range:NSMakeRange(0,source.length) withTemplate:tpl];
    NSError *error=nil;id<MTLLibrary> lib=variant_library(source,&error);id<MTLFunction> fn=lib?[lib newFunctionWithName:base.name]:nil;
    if(!fn)MDIE("mesh empty export %s: %s",ms->key,error.localizedDescription.UTF8String);
    fn_note_src(fn,source);cache[key]=fn;return fn;
}
/* The phone's fallback (user 2026-11-10: "这个是兜底 必须有", "不允许奔溃"): the phone never compiles a mesh pipeline. Its
   pipeline archive has no mesh pipelines (mtk_build makes them with none, from functions written at run time), and compiling
   them there took about 100 s of warm-up and once crashed MTLCompilerService (SIGBUS in AGX code generation, 2026-11-10).
   So on the phone a mesh draw is skipped and counted (draw_skipped), the warm-up leaves the M rows, and a pipeline Metal
   refuses is a skip too, never an exit. OOT_IOS_MESH_COMPILE=1 compiles them on the phone as before, to compare. */
static int mtk_device_compiles(void) {
#if TARGET_OS_IPHONE
    static _Atomic int on=-1;int v=atomic_load_explicit(&on,memory_order_relaxed);
    if(v<0){const char *e=getenv("OOT_IOS_MESH_COMPILE");v=e&&atoi(e)!=0;
        if(atomic_exchange(&on,v)<0)MLOG("[mesh] the phone %s mesh pipelines (OOT_IOS_MESH_COMPILE=%s)",v?"compiles":"skips the draws of",e?e:"0");}
    return v;
#else
    return 1;
#endif
}
static void mtk_note_skip(NvShader *ms) {
    static _Atomic uint64_t skipped;uint64_t k=atomic_fetch_add(&skipped,1)+1;
    if(k<=4||k%1000==0)MLOG("【红】手机不现编网格管线：跳过这次网格绘制（mesh %s，第 %llu 次）",ms?ms->key:"-",(unsigned long long)k);
}
/* The pipeline archive's key of a mesh pipeline: the M row's pipeline key (as pso_user_record writes it), its task shader and
   the variant bits; nil for a diagnostic's pipeline. Until 2026-11-11 mesh pipelines were never archived: OOT's 124 listed
   ones were compiled at every start. The key only finds the file; Metal checks the functions and state. */
static NSString *mtk_archive_key(NvShader *ts,NvShader *ms,NvShader *fs,const mtk_pso_key *key) {
    if(g_mtk_geometry||oot_umul_diagnostic_on())return nil;
    pso_skey sk;pso_skey_from(&key->p,ms,fs,0,NULL,&sk);memset(&sk.fz_desc,0,sizeof sk.fz_desc);
    NSMutableData *b=[NSMutableData dataWithBytes:&sk length:sizeof sk];
    const char *task=ts?ts->key:"-";[b appendBytes:task length:strlen(task)+1];
    uint64_t variant=key->variant;[b appendBytes:&variant length:sizeof variant];
    return [@"m" stringByAppendingString:par_hash(b.bytes,b.length)];
}
/* The mesh pipeline for a filled key, on any thread (the first draw, the warm-up, or the packaging fill); nil and *error when
   Metal refuses it. Taken from the pipeline archive when its file holds it (*from_archive = 1); otherwise compiled, and a
   warm-up or fill thread writes it to the archive (par_add), as pso_new does for render pipelines. */
static id<MTLRenderPipelineState> mtk_build(NvShader *ts,NvShader *ms,NvShader *fs,const mtk_pso_key *key,id<MTLFunction> base,int pack,NSError **error,int *from_archive) {
    if(from_archive)*from_archive=0;
    id<MTLFunction> mesh=mtk_mesh_fn_base(ms,fs,base),packedFs=nil;
    if(pack)mesh=mtk_pack_varyings(ms,fs,mesh,key->p.dfmt==0,&packedFs);   // nvn_mesh_pack.inc: fewer declared vec4 slots, same values
    MTLRenderPipelineDescriptor *d=pso_desc(&key->p,ms,fs,packedFs,0);
    MTLMeshRenderPipelineDescriptor *m=[MTLMeshRenderPipelineDescriptor new];
    m.label=[NSString stringWithFormat:@"task=%s mesh=%s fs=%s",ts?ts->key:"none",ms->key,fs?fs->key:"none"];
    if(g_mtk_geometry)mesh=mtk_geo_function(ms,fs,mesh);
    m.meshFunction=mtk_empty_export_fn(mesh,ms);m.fragmentFunction=d.fragmentFunction;m.maxTotalThreadsPerMeshThreadgroup=ms->mesh_threads;
    if(!m.meshFunction.label.length)m.meshFunction.label=@(ms->key);
    if(ts){m.objectFunction=ts->fn;m.payloadMemoryLength=(ts->mesh_payload+15u)&~15u;m.maxTotalThreadsPerObjectThreadgroup=ts->mesh_threads;}
    m.rasterSampleCount=d.rasterSampleCount;m.alphaToCoverageEnabled=d.alphaToCoverageEnabled;
    for(int i=0;i<8;i++)m.colorAttachments[i]=d.colorAttachments[i];
    m.depthAttachmentPixelFormat=d.depthAttachmentPixelFormat;m.stencilAttachmentPixelFormat=d.stencilAttachmentPixelFormat;
    NSString *akey=mtk_archive_key(ts,ms,fs,key);
    id<MTLBinaryArchive> a=akey?par_archive_for(g_dev,akey):nil;
    if(a){
        m.binaryArchives=@[a];
        id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:nil];
        par_pre_note(ps!=nil);m.binaryArchives=nil;
        if(ps){par_count(1);atomic_fetch_add(&g_mtk_archive_hits,1);if(from_archive)*from_archive=1;return ps;}
    }
    if(akey&&g_par_on){par_count(0);atomic_fetch_add(&g_mtk_archive_misses,1);}
    id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:error];
    if(ps&&akey)par_add(g_dev,akey,m);
    return ps;
}
static id<MTLRenderPipelineState> mtk_pso(NvProg *raster) {
    NvShader *ts=g_mtk.ts,*ms=g_mtk.ms,*fs=raster->sh[1];
    int selected=0;id<MTLFunction> base=mtk_translation_function(ms,&selected);g_mesh_translation_hits[selected]++;
    static int stats=-1;if(stats<0){const char*s=getenv("OOT_MESH_TRANSLATION_STATS");stats=s&&atoi(s);}
    if(stats&&g_mesh_translation_hits[selected]%600==1)MLOG("[mesh-translation] selected mode=%d draws=%llu loaded=%llu",selected,g_mesh_translation_hits[selected],g_mesh_translation_loaded[selected]);
    mtk_pso_key key; memset(&key,0,sizeof key);
    pso_key_fill(&key.p,ms,fs,g_pass.cfmt,g_pass.dfmt,g_pass.sfmt,g_pass.samples,0);
    memset(key.p.attr,0,sizeof key.p.attr);memset(key.p.stream,0,sizeof key.p.stream);
    key.task=(__bridge void *)(ts?ts->fn:nil);
    int pack=mtk_pack_on()&&!g_mtk_geometry&&!((key.p.a2c&PSO_HDR)&&fs&&fs->hdr_fn);   // the geometry capture diagnostic and the HDR twin read the unpacked layout
    key.variant=mtk_empty_export_on()|(g_mtk_geometry?2u:0u)|(pack?4u:0u)|((uint64_t)selected<<3);
    id<MTLRenderPipelineState> ps=hm_get_id(&g_mtk_cache,&key);if(ps)return ps;
    uint64_t build_t0=bp_now();NSError *error=nil;int archived=0;ps=mtk_build(ts,ms,fs,&key,base,pack,&error,&archived);
#if TARGET_OS_IPHONE
    if(!ps){MLOG("【红】网格管线没建成，跳过这次绘制（mesh %s）：%s",ms->key,error.localizedDescription.UTF8String);return nil;}   // the fallback: never an exit on the phone
#endif
    if(!ps)MDIE("task/mesh pipeline %s / %s: %s",ts?ts->key:"-",ms->key,error.localizedDescription.UTF8String);
    hm_put_id(&g_mtk_cache,&key,ps);MLOG("[mesh] %s task %s mesh %s fragment %s in %.1f ms at the draw",archived?"from the archive":"compiled",ts?ts->key:"-",ms->key,fs?fs->key:"-",(bp_now()-build_t0)/1e6);
    if(!g_mtk_geometry){   // the next start builds it ahead (mtk_warm_row); the geometry diagnostic's variant is not a pipeline of the game
        pso_skey sk;char hex[sizeof sk*2+1];pso_skey_from(&key.p,ms,fs,0,NULL,&sk);pso_skey_hex(&sk,hex);
        pso_user_record("M\t%s\t%s\n",ts?ts->key:"-",hex);
    }
    return ps;
}
/* An M row of a warm list: the pipeline a first draw would build (mtk_pso), on a warm-up thread. A first draw of one took the
   submit thread seconds on a cold Metal cache, 49-50 of them in a run (OOT 2026-11-09). Rows naming a diagnostic's variant are
   not built: those paths keep a static table without a lock. The claim (warm_claim) files it under the live shader objects. */
typedef struct { NvShader *ts,*ms,*fs; NSString *tkey,*mkey,*fkey; mtk_pso_key key; } mtk_row;
/* An M row's shaders and pipeline key, as the first draw makes them (mtk_pso): 1 filled, 0 its shaders are missing (counted
   as failed), -1 a row left alone (a diagnostic's variant, or no pipeline key). */
static int mtk_row_parse(NSArray<NSString *> *c,NSMutableDictionary<NSString *,NvShader *> *dict,mtk_row *r) {
    if(c.count<3||g_mtk_geometry||mtk_empty_export_on()||mtk_translation_mode())return -1;
    pso_skey sk;if(pso_skey_parse(c[2].UTF8String,&sk)||sk.fz||sk.merged)return -1;
    r->mkey=warm_key(sk.vs,sizeof sk.vs);r->fkey=warm_key(sk.fs,sizeof sk.fs);r->tkey=[c[1] isEqualToString:@"-"]?nil:c[1];
    r->ms=r->mkey.length?warm_shader(dict,r->mkey,sk.vs_stage):nil;r->fs=r->fkey.length?warm_shader(dict,r->fkey,sk.fs_stage):nil;
    r->ts=r->tkey?warm_shader(dict,r->tkey,3):nil;
    if(!r->ms||r->ms->mesh_kind!=2||(r->fkey.length&&!r->fs)||(r->tkey&&!r->ts))return 0;
    memset(&r->key,0,sizeof r->key);r->key.p=sk.k;r->key.p.vs=(__bridge void *)r->ms;r->key.p.fs=(__bridge void *)r->fs;r->key.p.fsfn=NULL;
    r->key.task=(__bridge void *)(r->ts?r->ts->fn:nil);
    return 1;
}
/* The pipeline of a parsed M row, with the variant bits a draw gives it (mtk_pso). */
static id<MTLRenderPipelineState> mtk_row_build(mtk_row *r,NSError **error,int *from_archive) {
    int selected=0;id<MTLFunction> base=mtk_translation_function(r->ms,&selected);
    int pack=mtk_pack_on()&&!((r->key.p.a2c&PSO_HDR)&&r->fs&&r->fs->hdr_fn);
    r->key.variant=(pack?4u:0u)|((uint64_t)selected<<3);
    return mtk_build(r->ts,r->ms,r->fs,&r->key,base,pack,error,from_archive);
}
static void mtk_warm_row(NSArray<NSString *> *c,NSMutableDictionary<NSString *,NvShader *> *dict) {
    if(!mtk_device_compiles()){   // the phone's fallback: its M rows are not compiled there
        static _Atomic int said;if(!atomic_exchange(&said,1))MLOG("预热：手机不现编网格管线，清单里的 M 行都跳过（兜底；OOT_IOS_MESH_COMPILE=1 才现编）");
        return;
    }
    mtk_row r={0};int parsed=mtk_row_parse(c,dict,&r);
    if(parsed<0)return;
    if(!parsed){atomic_fetch_add(&g_warm_mesh_failed,1);return;}
    NSError *error=nil;uint64_t started=bp_now();id<MTLRenderPipelineState> ps=mtk_row_build(&r,&error,NULL);
    atomic_fetch_add(&g_warm_pipeline_ns,bp_now()-started);atomic_fetch_add(&g_warm_pipeline_count,1);
    if(!ps){atomic_fetch_add(&g_warm_mesh_failed,1);MLOG("【红】预热：网格管线没建成（mesh %s）：%s",r.ms->key,error.localizedDescription.UTF8String);return;}
    WarmItem *item=[WarmItem new];item->kind=3;item->key=r.tkey;item->vs_key=r.mkey;item->fs_key=r.fkey;item->shader=r.ts;item->vs=r.ms;item->fs=r.fs;
    item->pso=r.key.p;item->mesh_variant=r.key.variant;item->object=ps;
    warm_enqueue(item);__atomic_fetch_add(&g_warm_rps,1,__ATOMIC_RELAXED);atomic_fetch_add(&g_warm_mesh,1);
}
/* An M row of a pipeline list into the archive (fill_row: the packaging tool, a first start's fill): 1 compiled and written,
   2 already in the archive, 0 failed, -1 a row the archive does not take here: the phone compiles no mesh pipeline, and the
   iOS packaging's device libraries (TOTK_SHADER_DEVICE_LIBS) carry none yet. */
static int mtk_fill_row(NSArray<NSString *> *c,NSMutableDictionary<NSString *,NvShader *> *dict) {
    if(!mtk_device_compiles()||getenv("TOTK_SHADER_DEVICE_LIBS"))return -1;
    mtk_row r={0};int parsed=mtk_row_parse(c,dict,&r);
    if(parsed<=0)return parsed;
    NSError *error=nil;int archived=0;id<MTLRenderPipelineState> ps=mtk_row_build(&r,&error,&archived);
    if(!ps){MLOG("【红】管线存档：网格管线没建成（mesh %s）：%s",r.ms->key,error.localizedDescription.UTF8String);return 0;}
    return archived?2:1;
}
/* Device stage memory for mesh shaders (translator MeshStage.cs, 2026-11-08; the constants are the same there): header
   word 0 a counter, 1 the slot mask, 2 the slot stride in words; one in-use flag a slot from word 4; the slots from word
   8192, 4096 words (16 KiB) each. A threadgroup takes a free slot (counter, then an exchange on the flag) and releases it
   after its exports, so a slot is never reused while its threadgroup runs; more live threadgroups than slots only wait.
   Why: as threadgroup memory the 11-16 KiB stage area let a core hold two or three mesh threadgroups (about 5 %
   occupancy, Instruments 2026-11-06); the village's four heaviest mesh shaders moved to a device scratch by hand took the
   GPU frame from 22.2 to 20.0 ms (measurement 2026-11-08). OOT_MESH_SCRATCH_SLOTS overrides the slot count (power of two). */
enum { MTK_SCRATCH_BUFFER=28, MTK_SCRATCH_FORCED=3, MTK_SCRATCH_FLAGS=4, MTK_SCRATCH_SLOTS=8192, MTK_SCRATCH_SLOT_WORDS=4096 };
static unsigned g_mtk_scratch_slots;
static id<MTLBuffer> g_mtk_scratch;
static uint64_t mtk_scratch_gpu(void) {
    if(!g_mtk_scratch){
        unsigned slots=TARGET_OS_IPHONE?1024:4096;{const char *e=getenv("OOT_MESH_SCRATCH_SLOTS");if(e&&atoi(e)>0)slots=(unsigned)atoi(e);}
        if(slots&(slots-1))MDIE("mesh stage scratch: %u slots is not a power of two",slots);
        if(MTK_SCRATCH_FLAGS+slots>MTK_SCRATCH_SLOTS)MDIE("mesh stage scratch: %u slot flags do not fit before the slots",slots);
        size_t bytes=((size_t)MTK_SCRATCH_SLOTS+(size_t)slots*MTK_SCRATCH_SLOT_WORDS)*4;
        g_mtk_scratch=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if(!g_mtk_scratch)MDIE("mesh stage scratch: %zu bytes",bytes);
        uint32_t *h=g_mtk_scratch.contents;memset(h,0,(size_t)MTK_SCRATCH_SLOTS*4);h[1]=slots-1;h[2]=MTK_SCRATCH_SLOT_WORDS;g_mtk_scratch_slots=slots;
        g_mtk_scratch.label=@"OOT mesh stage memory";res_add(g_mtk_scratch);
        MLOG("[mesh] device stage memory: %u slots of %u KiB (%zu MiB)",slots,MTK_SCRATCH_SLOT_WORDS*4/1024,bytes>>20);
    }
    return g_mtk_scratch.gpuAddress;
}
/* Every 600 frames (and on demand with flag mesh_batch_stats): slots the GPU holds right now (a racy read of the shared
   flags; a count that only grows is a leak), the counter, and threadgroups that gave up waiting for a free slot (the
   generated wrapper's spin limit; must stay 0). */
static void mtk_scratch_report(void) {
    if(!g_mtk_scratch)return;
    static uint64_t last;uint64_t f=atomic_load(&g_cur_frame);if(f<last+600&&!atomic_load(&g_mesh_batch_dump))return;last=f;
    const uint32_t *h=g_mtk_scratch.contents;unsigned held=0;for(unsigned i=0;i<g_mtk_scratch_slots;i++)held+=h[MTK_SCRATCH_FLAGS+i]!=0;
    MLOG("[mesh] device stage memory F%llu: slots held now %u of %u, counter %u, forced takes %u",(unsigned long long)f,held,g_mtk_scratch_slots,h[0],h[MTK_SCRATCH_FORCED]);
    if(h[MTK_SCRATCH_FORCED]){static int said;if(said++<3)MLOG("【红】[mesh] %u threadgroups took a slot still in use (spin limit): stage memory may have been shared",h[MTK_SCRATCH_FORCED]);}
}
static void mtk_encode(id<MTL4RenderCommandEncoder> en) {
    NvShader *ts=g_mtk.ts,*ms=g_mtk.ms;
    if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_MESH_TABLES,.u={!!ts}},0);
    else {[en setArgumentTable:g_vt atStages:MTLRenderStageMesh];if(ts)[en setArgumentTable:g_ot atStages:MTLRenderStageObject];}
    uint64_t first=g_mtk.first_gpu;
    if(!first){id<MTLBuffer>b;size_t o=arena_alloc(16,&b);memcpy((uint8_t *)b.contents+o,&g_mtk.first,4);first=b.gpuAddress+o;}
    if(ts){g_args_dirty[3]=1;encode_args((id<MTLCommandEncoder>)en,ts,3,0);
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_OBJECT_ADDRESS,.u={29,first}},0);else[g_ot setAddress:first atIndex:29];}
    g_args_dirty[4]=1;encode_args((id<MTLCommandEncoder>)en,ms,4,1);enc_r_vbuf(en,29,first);
    enc_r_vbuf(en,MTK_SCRATCH_BUFFER,mtk_scratch_gpu());mtk_scratch_report();   // device stage memory of the mesh shaders the translator put there (MeshStage.cs)
    if(mtk_geo_selected(g_mtk.fs))enc_r_vbuf(en,30,g_mtk_geometry?g_mtk_geometry.gpuAddress:zero_bank_gpu());
    int timing=ms_time_begin(en,ms->key,ts?ts->key:NULL,g_mtk.fs?g_mtk.fs->key:NULL,g_mtk.indirect_gpu,g_mtk.first,g_mtk.count,ms->mesh_threads,g_mtk.profile_grid);
    if(g_mtk.indirect_gpu){
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_DRAW_MESH_INDIRECT,.u={g_mtk.indirect_gpu,ts?ts->mesh_threads:1,ms->mesh_threads}},1);
        else [en drawMeshThreadgroupsWithIndirectBuffer:g_mtk.indirect_gpu threadsPerObjectThreadgroup:MTLSizeMake(ts?ts->mesh_threads:1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(ms->mesh_threads,1,1)];
    }
    else if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_DRAW_MESH,.u={g_mtk.count,ts?ts->mesh_threads:1,ms->mesh_threads}},1);
    else [en drawMeshThreadgroups:MTLSizeMake(g_mtk.count,1,1) threadsPerObjectThreadgroup:MTLSizeMake(ts?ts->mesh_threads:1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(ms->mesh_threads,1,1)];
    ms_time_stamp(en,timing,1);
}
static void mesh_tasks_draw(const p_draw *dw) {
    if(!S.prog||dw->count<=0)return;
    NvProg *pr=prog_get(S.prog);if(!pr)MDIE("mesh draw has no program");
    int fit=prog_fit(pr);
#if TARGET_OS_IPHONE
    /* A variant the phone has no translation for (ios/tools: only the listed shaders, no translator yet) is skipped and counted
       like any other draw whose shader is not ready (draw_skipped), not fatal; the request is recorded for fill_shaders.py. */
    if(fit<0||pr->missing){draw_skipped();return;}
#endif
    if(fit<0)MDIE("mesh shader translation is unavailable");
    NvShader *ts=pr->sh[3],*ms=pr->sh[4];
    if(ts&&ts->mesh_kind!=1)MDIE("task stage is not a task shader");
#if TARGET_OS_IPHONE
    if(!ms||!ms->fn||(ts&&!ts->fn)){draw_skipped();return;}
    if(!mtk_device_compiles()){mtk_note_skip(ms);draw_skipped();return;}   // the fallback: no mesh pipeline is made on the phone
#endif
    if(!ms||ms->mesh_kind!=2||!ms->fn||(ts?!ts->fn:!ms->mesh_notask))MDIE("DrawMeshTasks without complete task/mesh stages");
    upd_guard_stage(ts,3);upd_guard_stage(ms,4);
    prepare_shader_pools(ts);prepare_shader_pools(ms);
    prepare_shader_images(ts,3);prepare_shader_images(ms,4);
    NvProg *raster=pr->geo_lean_ras;
    if(!raster||raster->sh[1]!=pr->sh[1]||raster->sh[0]){raster=[NvProg new];raster->sh[1]=pr->sh[1];pr->geo_lean_ras=raster;}
    { extern _Atomic uint32_t g_skip_ms; uint32_t sk=atomic_load_explicit(&g_skip_ms,memory_order_relaxed);   // diagnostic "flag skip_ms": leave out this mesh shader's draws
      if(sk){char h[8];memcpy(h,ms->key,7);h[7]=0;if((uint32_t)strtoul(h,NULL,16)==sk)return;} }
    g_mtk.on=1;g_mtk.ts=ts;g_mtk.ms=ms;g_mtk.fs=pr->sh[1];g_mtk.first=(uint32_t)dw->first;g_mtk.count=(uint32_t)dw->count;
    g_mtk.first_gpu=g_mtk.indirect_gpu=0;
    g_mtk.profile_grid=-1;
    if(dw->ind){
        /* NV_mesh_shader indirect ABI: taskCount, firstTask. Convert the one
           dimensional grid on the GPU timeline; the first-task field is bound
           directly, so a prior compute producer never needs a CPU rendezvous. */
        void *src;uint64_t off,gpu;upd_guard(dw->ind,dw->ind+8);
        if(!mtl_gpu_addr(dw->ind,8,&src,&off,&gpu))MDIE("mesh indirect arguments are not mapped");
        use_rec("buf",dw->ind,8,0,NULL);
        g_mtk.indirect_gpu=mesh_batch_grid(dw->ind,&g_mtk.first_gpu);
        if(!g_mtk.indirect_gpu){g_mtk.indirect_gpu=mesh_indirect_grid(dw->ind,(__bridge id<MTLBuffer>)src,off);g_mtk.first_gpu=gpu+4;}
        g_mtk.profile_grid=ms_grid_capture(g_mtk.indirect_gpu);
        static int read;static const char *dump;static int left=16;
        if(!read){read=1;dump=getenv("OOT_MESH_INDIRECT_DUMP");}
        if(dump&&left-->0){id<MTLBuffer> capture;size_t ca=arena_alloc(16,&capture);
            enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)src,off,capture,ca,8);
            NSString *path=[NSString stringWithFormat:@"%s-%d.bin",dump,left];
            [g_commit_blocks addObject:^{[[NSData dataWithBytes:(uint8_t *)capture.contents+ca length:8]writeToFile:path atomically:YES];}];
        }
    }
    mtk_geo_prepare(ms,pr->sh[1]);mtk_capture_storage(pr);
    g_geo_ras.pr=raster;g_pso_dirty=1;
    p_draw tri={.prim=4,.count=3,.instances=1};draw_inner(&tri,NULL);
    g_geo_ras.pr=nil;g_mtk.on=0;args_dirty_all();g_pso_dirty=1;
    mesh_indirect_written(pr);mesh_indirect_stats();
    mesh_batch_written(pr);
    static int once;if(!once++)MLOG("[mesh] first native draw: %u workgroups from %u",g_mtk.count,g_mtk.first);
}
