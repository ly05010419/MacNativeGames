/* Experimental, opt-in consumer of offline typed shader address proofs.
   A certificate specializes a shader to exact immutable uniform words and a
   direct dispatch. No live CPU source or observed GPU page set authorizes it.
   Unknown inputs, changed words, indirect dispatch and other stages fall back
   to the full address range. This does not alter guest writes or retirement. */
static uint64_t g_write_domain_hits,g_write_domain_misses,g_write_domain_verified;
static int raw_write_domain_on(void) {
    int v=atomic_load(&g_raw_write_domain);
    if(v<0){const char *s=getenv("OOT_RAW_WRITE_DOMAIN");v=!TARGET_OS_IPHONE&&s&&atoi(s)!=0;atomic_store(&g_raw_write_domain,v);}
    return !TARGET_OS_IPHONE&&v;
}
/* Every guard word equals the immutable uniform bytes bound to this dispatch. */
static int raw_write_guards_match(NvShader *sh,NSArray *guards,int stage,const uint64_t *slots,int resolved) {
    for(NSDictionary *g in guards){
        if(![g isKindOfClass:NSDictionary.class]||![g[@"reg"] isKindOfClass:NSNumber.class]||![g[@"offset"] isKindOfClass:NSNumber.class]||![g[@"word"] isKindOfClass:NSNumber.class])return 0;
        uint64_t reg=[g[@"reg"] unsignedLongLongValue],off=[g[@"offset"] unsignedLongLongValue],word=[g[@"word"] unsignedLongLongValue];
        if(reg>=16||off>65532||off%4||word>UINT32_MAX)return 0;
        const p_bind_buf *u=&S.ubo[stage][reg];if(u->size<off+4||u->size>65536)return 0;
        int matched=0;NSNumber *arg=g[@"arg_id"];
        if(arg&&(![arg isKindOfClass:NSNumber.class]||arg.unsignedLongLongValue>=NVN_REFL_MAXB))return 0;
        for(int i=0;i<resolved;i++){
            const nvn_rbind *b=&sh->rf.b[i];
            if(b->reg!=reg||b->aid>=NVN_REFL_MAXB)continue;
            if(arg){if(b->kind!=NVN_RB_CBUF||b->aid!=arg.unsignedLongLongValue)continue;}
            else if(b->kind!=NVN_RB_CBUF&&b->kind!=NVN_RB_UBOP)continue;
            const uint8_t *bytes=bound_uniform_bytes(slots[b->aid],(size_t)u->size,nil);
            uint32_t actual=0;if(!bytes)return 0;memcpy(&actual,bytes+off,4);if(actual!=word)return 0;matched=1;
        }
        if(!matched)return 0;
    }
    return 1;
}
static int raw_write_domain_match(NvShader *sh,NSDictionary *d,int stage,const uint64_t *slots,int resolved) {
    if(stage!=5||!g_dispatch_domain_direct||g_dispatch_domain_simd!=[d[@"simd_width"] intValue])return 0;
    if(![d[@"simd_width"] isKindOfClass:NSNumber.class])return 0;
    NSArray *grid=d[@"grid"],*local=d[@"local"],*guards=d[@"guards"],*ranges=d[@"high_intervals"];
    if(![grid isKindOfClass:NSArray.class]||grid.count!=3||![local isKindOfClass:NSArray.class]||local.count!=3||
       ![guards isKindOfClass:NSArray.class]||!guards.count||guards.count>256||![ranges isKindOfClass:NSArray.class]||!ranges.count||ranges.count>64)return 0;
    for(int k=0;k<3;k++)if(![grid[k] isKindOfClass:NSNumber.class]||![local[k] isKindOfClass:NSNumber.class]||[grid[k] unsignedLongLongValue]!=g_dispatch_grid[k]||[local[k] unsignedLongLongValue]!=sh->rf.tg[k])return 0;
    if(!raw_write_guards_match(sh,guards,stage,slots,resolved))return 0;
    uint64_t last=0;int first=1;
    for(NSArray *r in ranges){
        if(![r isKindOfClass:NSArray.class]||r.count!=2||![r[0] isKindOfClass:NSNumber.class]||![r[1] isKindOfClass:NSNumber.class])return 0;
        uint64_t a=[r[0] unsignedLongLongValue],b=[r[1] unsignedLongLongValue];
        if(a>b||b>=UINT32_MAX||(!first&&a<=last))return 0;last=b;first=0;
    }
    return 1;
}
#include "nvn_raw_write_auto.inc"
static NSArray *raw_write_domain_bytes(NvShader *sh,NSDictionary *d,int stage,const uint64_t *slots,int resolved);
void mtl_note_exact_drop(uint64_t a, uint64_t b, const char *key);
/* The GPU oracle consumes the same frozen uniform whose guard was checked.
   This tests the binding/guard gate, not the offline address analyzer. */
static int raw_write_domain_probe(void) {
    int checked=0,old_direct=g_dispatch_domain_direct,old_simd=g_dispatch_domain_simd;
    uint32_t old_grid[3];memcpy(old_grid,g_dispatch_grid,sizeof old_grid);
    p_bind_buf old_ubo=S.ubo[5][0];
    NvShader *sh=[NvShader new];sh->stage=5;sh->rf.nb=1;sh->rf.tg[0]=128;sh->rf.tg[1]=sh->rf.tg[2]=1;
    sh->rf.b[0]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0,.aid=0};
    NSDictionary *proof=@{@"grid":@[@169,@1,@1],@"local":@[@128,@1,@1],@"simd_width":@32,
        @"guards":@[@{@"reg":@0,@"offset":@0,@"word":@0x12345678}],@"high_intervals":@[@[@14,@19]]};
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void k(device const uint *u [[buffer(0)]],device uint *o [[buffer(1)]],uint i [[thread_position_in_grid]]){if(i<4)o[i]=u[i]^0xa5a5a5a5u;}" options:nil error:&error];
    id<MTLComputePipelineState> ps=lib?[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&error]:nil;
    if(!ps)MDIE("write-domain guard GPU probe: %s",error.description.UTF8String);
    id<MTLBuffer> live=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared],out=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];res_add(live);res_add(out);
    int negative=getenv("OOT_WRITE_DOMAIN_PROBE_NEGATIVE")?atoi(getenv("OOT_WRITE_DOMAIN_PROBE_NEGATIVE")):0;
    for(int step=0;step<4;step++){
        uint32_t expected[4]={0x12345678,17u+(uint32_t)step,0xdeadbeef,0xffffffff};memcpy(live.contents,expected,16);
        op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        __unsafe_unretained id<MTLBuffer> arena=nil;uint8_t *cpu=NULL;uint64_t gpu=0;arena_alloc_u(16,&arena,&cpu,&gpu);memcpy(cpu,expected,16);
        S.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)live.contents,.size=16};uint64_t slots[NVN_REFL_MAXB]={gpu};
        g_dispatch_grid[0]=169;g_dispatch_grid[1]=g_dispatch_grid[2]=1;g_dispatch_domain_direct=1;g_dispatch_domain_simd=32;
        if(!raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain frozen gate rejected");checked++;
        ((uint32_t *)live.contents)[0]^=1;
        if(!raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain used changed live input");checked++;
        sh->rf.nb=2;sh->rf.b[1]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0,.aid=1};
        uint64_t alias_slots[NVN_REFL_MAXB]={gpu,live.gpuAddress};
        NSMutableDictionary *typed=[proof mutableCopy];
        typed[@"guards"]=@[@{@"reg":@0,@"arg_id":@0,@"offset":@0,@"word":@0x12345678}];
        if(!raw_write_domain_match(sh,typed,5,alias_slots,2))MDIE("write-domain lost exact argument identity");checked++;
        typed[@"guards"]=@[@{@"reg":@0,@"arg_id":@1,@"offset":@0,@"word":@0x12345678}];
        if(raw_write_domain_match(sh,typed,5,alias_slots,2))MDIE("write-domain accepted a different argument");checked++;
        typed[@"guards"]=@[@{@"reg":@0,@"arg_id":@0,@"offset":@0,@"word":@0x12345678}];
        alias_slots[0]=live.gpuAddress;alias_slots[1]=gpu;
        if(raw_write_domain_match(sh,typed,5,alias_slots,2))MDIE("write-domain matched another slot's equal word");checked++;
        sh->rf.nb=1;
        uint64_t badslots[NVN_REFL_MAXB]={live.gpuAddress};
        if(raw_write_domain_match(sh,proof,5,badslots,1))MDIE("write-domain accepted live binding");checked++;
        ((uint32_t *)cpu)[0]^=1;
        if(raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain accepted changed frozen word");checked++;
        ((uint32_t *)cpu)[0]^=1;
        g_dispatch_domain_direct=0;if(raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain accepted indirect grid");checked++;
        g_dispatch_domain_direct=1;g_dispatch_grid[0]++;if(raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain accepted changed grid");checked++;
        g_dispatch_grid[0]--;g_dispatch_domain_simd=64;if(raw_write_domain_match(sh,proof,5,slots,1))MDIE("write-domain accepted different simd width");checked++;
        g_dispatch_domain_simd=32;if(raw_write_domain_match(sh,proof,1,slots,1)||raw_write_domain_match(sh,proof,5,slots,0))MDIE("write-domain accepted unknown stage/input");checked++;
        {   // exact byte refinement: accepted only with its own guards, inside a proved stripe, ordered
            uint64_t s14=UINT64_C(14)<<32;
            NSMutableDictionary *exact=[proof mutableCopy];
            exact[@"byte_intervals"]=@[@[@(s14+0x100),@(s14+0x204)],@[@(s14+0x1000),@(s14+0x1004)]];
            exact[@"byte_guards"]=@[@{@"reg":@0,@"offset":@0,@"word":@0x12345678},@{@"reg":@0,@"offset":@4,@"word":@(17u+(uint32_t)step)}];
            if(!raw_write_domain_match(sh,exact,5,slots,1)||!raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain rejected exact bytes");checked++;
            ((uint32_t *)cpu)[1]^=1;
            if(!raw_write_domain_match(sh,exact,5,slots,1))MDIE("write-domain stripe guard depends on the low word");checked++;
            if(raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain accepted bytes with a changed low-word guard");checked++;
            ((uint32_t *)cpu)[1]^=1;
            exact[@"byte_intervals"]=@[@[@((UINT64_C(20)<<32)+0x100),@((UINT64_C(20)<<32)+0x104)]];
            if(raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain accepted bytes outside the stripes");checked++;
            exact[@"byte_intervals"]=@[@[@(s14+0x1000),@(s14+0x1004)],@[@(s14+0x100),@(s14+0x104)]];
            if(raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain accepted unordered bytes");checked++;
            exact[@"byte_intervals"]=@[@[@(s14+0x104),@(s14+0x104)]];
            if(raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain accepted an empty byte range");checked++;
            [exact removeObjectForKey:@"byte_guards"];exact[@"byte_intervals"]=@[@[@(s14+0x100),@(s14+0x104)]];
            if(raw_write_domain_bytes(sh,exact,5,slots,1))MDIE("write-domain accepted bytes without guards");checked++;
        }
        id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,ps);enc_c_args(ce,negative?live.gpuAddress:gpu,0);enc_c_args(ce,out.gpuAddress,1);
        [ce dispatchThreads:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(4,1,1)];enc_c_end(ce);mtl_sub_commit();mtl_finish();
        for(int k=0;k<4;k++){if(((uint32_t *)out.contents)[k]!=(expected[k]^0xa5a5a5a5u))MDIE("write-domain GPU did not consume proved input step%d word%d",step,k);checked++;}
    }
    S.ubo[5][0]=old_ubo;g_dispatch_domain_direct=old_direct;g_dispatch_domain_simd=old_simd;memcpy(g_dispatch_grid,old_grid,sizeof old_grid);
    MLOG("[probe-gpu-write-domain] pass %d checks; frozen guard/input gate only",checked);return checked;
}
/* Exact byte refinement of a matched proof (analyzer 2026-11-08): the low address
   word's range narrows each 4-GiB stripe to the bytes actually written. It has its
   own guards (the extra uniform words the low analysis read); when they do not
   match this dispatch, the stripes stand. Each range must lie in a proved stripe. */
static uint64_t g_write_domain_exact,g_write_domain_stripe;
static uint64_t g_spared_lo[48];static int g_nspared;
static NSArray *raw_write_domain_bytes(NvShader *sh,NSDictionary *d,int stage,const uint64_t *slots,int resolved) {
    NSArray *bytes=d[@"byte_intervals"],*guards=d[@"byte_guards"],*stripes=d[@"high_intervals"];
    if(![bytes isKindOfClass:NSArray.class]||!bytes.count||bytes.count>64||![guards isKindOfClass:NSArray.class]||!guards.count||guards.count>256)return nil;
    uint64_t last=0;
    for(NSArray *r in bytes){
        if(![r isKindOfClass:NSArray.class]||r.count!=2||![r[0] isKindOfClass:NSNumber.class]||![r[1] isKindOfClass:NSNumber.class])return nil;
        uint64_t a=[r[0] unsignedLongLongValue],b=[r[1] unsignedLongLongValue];int inside=0;
        if(a>=b||a<last)return nil;last=b;
        for(NSArray *s in stripes)if((a>>32)>=[s[0] unsignedLongLongValue]&&((b-1)>>32)<=[s[1] unsignedLongLongValue]){inside=1;break;}
        if(!inside)return nil;
    }
    return raw_write_guards_match(sh,guards,stage,slots,resolved)?bytes:nil;
}
static int raw_write_domain_drop(NvShader *sh,int stage,const uint64_t *slots,int resolved,id<MTLBuffer> log,size_t offset) {
    if((!raw_write_domain_on()&&!raw_write_auto_on())||stage!=5||totk_vpool_aliases_used())return 0;
    if(!sh->raw_write_domains_loaded){
        sh->raw_write_domains_loaded=1;
        const char *dir=getenv("OOT_RAW_WRITE_DOMAIN_DIR");
        if(dir){
            NSString *path=[NSString stringWithFormat:@"%s/%s.write-domain.json",dir,sh->key];
            NSData *data=[NSData dataWithContentsOfFile:path];
            NSDictionary *j=data&&data.length<262144?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
            if([j isKindOfClass:NSDictionary.class]&&[j[@"abi"] intValue]==1&&[j[@"chain"] isEqual:@DMSH_CHAIN]&&[j[@"shader"] isEqual:@(sh->key)]){
                NSData *src=[sh_src(sh) dataUsingEncoding:NSUTF8StringEncoding];
                unsigned char hash[32];char hex[65]={0};if(src){CC_SHA256(src.bytes,(CC_LONG)src.length,hash);for(int k=0;k<32;k++)snprintf(hex+2*k,3,"%02x",hash[k]);}
                NSData *reflection=totk_rt_read([[sh->src_path stringByDeletingPathExtension] stringByAppendingPathExtension:@"json"]);
                char rh[65]={0};if(reflection){CC_SHA256(reflection.bytes,(CC_LONG)reflection.length,hash);for(int k=0;k<32;k++)snprintf(rh+2*k,3,"%02x",hash[k]);}
                if(src&&reflection&&[j[@"reflection_sha256"] isEqual:@(rh)]&&[j[@"msl_sha256"] isEqual:@(hex)]&&[j[@"variants"] isKindOfClass:NSArray.class]&&[j[@"variants"] count]<=16)sh->raw_write_domains=j[@"variants"];
            }
        }
    }
    NSDictionary *match=nil;
    for(NSDictionary *d in (raw_write_domain_on()?sh->raw_write_domains:nil))if([d isKindOfClass:NSDictionary.class]&&raw_write_domain_match(sh,d,stage,slots,resolved)){match=d;break;}
    if(raw_write_auto_on())raw_write_auto_collect();
    if(!match&&raw_write_auto_on())for(NSDictionary *d in sh->raw_write_auto_domains)if(raw_write_domain_match(sh,d,stage,slots,resolved)){match=d;break;}
    if(!match)match=raw_write_auto_prove(sh,stage,slots,resolved);
    if(!match){g_write_domain_misses++;if(g_write_domain_misses<=4)MLOG("[write-domain] fallback %.16s direct=%d simd=%d variants=%lu",sh->key,g_dispatch_domain_direct,g_dispatch_domain_simd,(unsigned long)sh->raw_write_domains.count);return 0;}
    NSArray *ranges=match[@"high_intervals"],*bytes=raw_write_domain_bytes(sh,match,stage,slots,resolved);
    if(!bytes&&match[@"byte_intervals"]&&raw_write_auto_on()){
        // Same stripes, different low-word inputs: one bounded new proof (the shader's input-identity budget applies).
        NSDictionary *fresh=raw_write_auto_prove(sh,stage,slots,resolved);
        NSArray *fb=fresh?raw_write_domain_bytes(sh,fresh,stage,slots,resolved):nil;
        if(fb){match=fresh;ranges=fresh[@"high_intervals"];bytes=fb;}
    }
    if(bytes){
        for(NSArray *r in bytes){uint64_t a=[r[0] unsignedLongLongValue],b=[r[1] unsignedLongLongValue];usnap_drop(a,b-a);mtl_note_exact_drop(a,b,sh->key);}g_write_domain_exact++;
        /* Diagnostic (2026-11-08 white frames): uniform snapshots inside the proved stripes that the exact bytes leave
           valid. flag exact_usnap_stripes 1 drops them as the stripes did (snapshots only; waits stay exact). */
        int layers=atomic_load(&g_exact_usnap_stripes_ext);
        for(NSArray *s in ranges){
            uint64_t a=[s[0] unsignedLongLongValue]<<32,b=([s[1] unsignedLongLongValue]+1)<<32;
            if(layers&2){g_pool_write_epoch++;pool_input_invalidate(a,b-a);}   // diagnostic: descriptor-source captures by stripes
            if(layers&4){usnap_drop(a,b-a);continue;}                           // diagnostic: the complete former stripe drop
            for(int i=usnap_first(a);i<g_nusnap&&g_usnap[i].lo<b;i++){
                if(atomic_load(&g_usnap[i].st)==US_DROPPED)continue;
                if(layers&1){if(atomic_exchange(&g_usnap[i].st,US_DROPPED)!=US_DROPPED)g_usnap_drops++;continue;}
                int seen=0;for(int k=0;k<g_nspared;k++)if(g_spared_lo[k]==g_usnap[i].lo){seen=1;break;}
                if(!seen&&g_nspared<48){g_spared_lo[g_nspared++]=g_usnap[i].lo;MLOG("[write-domain-spared] writer=%.16s snapshot lo=%#llx size=%llu frame=%llu",sh->key,(unsigned long long)g_usnap[i].lo,(unsigned long long)(g_usnap[i].hi-g_usnap[i].lo),(unsigned long long)atomic_load(&g_cur_frame));}
            }
        }
    }
    else {for(NSArray *r in ranges){uint64_t a=[r[0] unsignedLongLongValue]<<32,b=([r[1] unsignedLongLongValue]+1)<<32;usnap_drop(a,b-a);}g_write_domain_stripe++;}
    g_write_domain_hits++;
    if(g_write_domain_hits<=4||atomic_load(&g_cur_frame)%300==0)MLOG("[write-domain] matched %.16s hits=%llu fallback=%llu verified=%llu exact=%llu stripe=%llu sbufu-pool-drops=%llu",sh->key,g_write_domain_hits,g_write_domain_misses,g_write_domain_verified,g_write_domain_exact,g_write_domain_stripe,g_sbufu_write_drops);
    const char *check=getenv("OOT_RAW_WRITE_DOMAIN_CHECK");
    if((check&&atoi(check))||raw_write_log_on()){
        ((uint32_t *)((uint8_t *)log.contents+offset))[GML_CONTROL_WORD]=1;
        [g_commit_blocks addObject:^{
            const uint32_t *p=(const uint32_t *)((const uint8_t *)log.contents+offset);
            if(p[0])MDIE("write-domain GPU page log overflow");
            for(int k=1;k<=1024;k++)if(p[k]){
                uint32_t high=p[k]>>20;int covered=0;uint64_t page=(uint64_t)p[k]<<12;
                if(bytes){for(NSArray *r in bytes)if(page<[r[1] unsignedLongLongValue]&&page+4096>[r[0] unsignedLongLongValue]){covered=1;break;}}
                else for(NSArray *r in ranges)if(high>=[r[0] unsignedLongLongValue]&&high<=[r[1] unsignedLongLongValue]){covered=1;break;}
                if(!covered)MDIE("write-domain GPU wrote outside proof: page=%#x",p[k]);
            }
            g_write_domain_verified++;if(g_write_domain_verified<=2||g_write_domain_verified%60==0)MLOG("[write-domain-check] GPU verified %llu dispatches",g_write_domain_verified);
        }];
    }
    return 1;
}
