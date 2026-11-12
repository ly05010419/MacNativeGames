/* NativeAOT interval analysis of compiler-attested SPIR-V. The proof consumes
   only the immutable uniform bytes actually bound to this dispatch. Its cached
   result is accepted through raw_write_domain_match, including all word guards. */
static int raw_write_auto_on(void) {
    int v=atomic_load(&g_raw_write_auto);
    if(v<0){const char *s=getenv("OOT_RAW_WRITE_AUTO");v=!TARGET_OS_IPHONE&&!(s&&!atoi(s));atomic_store(&g_raw_write_auto,v); /* Mac default on since 2026-11-08; OOT_RAW_WRITE_AUTO=0 turns it off */ }
    return !TARGET_OS_IPHONE&&v;
}
static os_unfair_lock g_raw_write_auto_lock=OS_UNFAIR_LOCK_INIT;static NSMutableArray *g_raw_write_auto_done;static _Atomic int g_raw_write_auto_ready;
static dispatch_queue_t raw_write_auto_queue(void) {
    static dispatch_queue_t q;static dispatch_once_t once;
    dispatch_once(&once,^{q=dispatch_queue_create("oot.raw-write-proof",dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,QOS_CLASS_UTILITY,0));});
    return q;
}
/* On the thread that replays dispatches: take the analyses the background queue finished. A proved result joins its
   shader's domains (matched per dispatch, word guards included); a failed one is only logged. */
static void raw_write_auto_collect(void) {
    if(!atomic_exchange(&g_raw_write_auto_ready,0))return;
    os_unfair_lock_lock(&g_raw_write_auto_lock);NSArray *done=g_raw_write_auto_done;g_raw_write_auto_done=nil;os_unfair_lock_unlock(&g_raw_write_auto_lock);
    for(NSArray *e in done){
        NvShader *sh=e[0];NSDictionary *result=e[1]==(id)NSNull.null?nil:e[1];double ms=[e[3] unsignedLongLongValue]*41.6667/1e6;
        if(![result[@"ok"] boolValue]){
            MLOG("[write-domain-auto] fallback %.16s attempt=%d reason=%s (background, %.0f ms)",sh->key,[e[2] intValue],[result[@"error"] description].UTF8String?:"invalid result",ms);continue;
        }
        if(!sh->raw_write_auto_domains)sh->raw_write_auto_domains=[NSMutableArray array];[sh->raw_write_auto_domains addObject:result];
        MLOG("[write-domain-auto] proved %.16s attempt=%d calls=%lu guards=%lu ranges=%s (background, %.0f ms)",sh->key,[e[2] intValue],(unsigned long)[result[@"calls"] count],(unsigned long)[result[@"guards"] count],[result[@"high_intervals"] description].UTF8String,ms);
    }
}
static NSDictionary *raw_write_auto_prove(NvShader *sh,int stage,const uint64_t *slots,int resolved) {
    if(!raw_write_auto_on()||stage!=5||!g_dispatch_domain_direct||g_dispatch_domain_simd!=32||totk_vpool_aliases_used())return nil;
    // Known storage-buffer bindings invalidate their declared ranges below.
    // Images and uniform-derived storage pointers have no complete byte extent
    // here; their writes must retain the full fallback.
    for(int i=0;i<sh->rf.nb;i++)if(sh->rf.b[i].kind==NVN_RB_IMG||(sh->rf.b[i].kind==NVN_RB_SBUFU&&sh->rf.b[i].rw))return nil;
    NSDictionary *cap=sh->raw_write_capsule;
    if(![cap isKindOfClass:NSDictionary.class]||[cap[@"abi"] intValue]!=1)return nil;
    if(!sh->raw_write_capsule_checked){
        sh->raw_write_capsule_checked=1;
        NSData *src=[sh_src(sh) dataUsingEncoding:NSUTF8StringEncoding];unsigned char hash[32];char hex[65]={0};
        if(src){CC_SHA256(src.bytes,(CC_LONG)src.length,hash);for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",hash[i]);}
        if(!src||![cap[@"msl_sha256"] isEqual:@(hex)]||![cap[@"spirv"] isKindOfClass:NSString.class]||[cap[@"spirv"] length]>2796204||![cap[@"bindings"] isKindOfClass:NSArray.class]||[cap[@"bindings"] count]>64){sh->raw_write_capsule=nil;return nil;}
    }
    static int loaded;static void *library;static int(*prove)(const char *,char *,int);
    if(!loaded){
        loaded=1;const char *override=getenv("OOT_WRITE_PROOF_LIBRARY");
        NSString *path=override?@(override):[[NSBundle.mainBundle resourcePath] stringByAppendingPathComponent:@"totk/build/shader-service/oot_write_proof.dylib"];
        library=dlopen(path.fileSystemRepresentation,RTLD_NOW|RTLD_LOCAL);if(library)prove=dlsym(library,"oot_raw_write_proof");
        MLOG("[write-domain-auto] native analyzer %s",prove?"loaded":"unavailable; full fallback");
    }
    if(!prove||sh->raw_write_auto_attempts>=16)return nil;
    NSMutableArray *inputs=[NSMutableArray array];CC_SHA256_CTX digest;CC_SHA256_Init(&digest);
    CC_SHA256_Update(&digest,g_dispatch_grid,sizeof g_dispatch_grid);CC_SHA256_Update(&digest,sh->rf.tg,sizeof sh->rf.tg);
    for(NSDictionary *binding in cap[@"bindings"]){
        if(![binding isKindOfClass:NSDictionary.class]||![binding[@"name"] isKindOfClass:NSString.class]||![binding[@"reg"] isKindOfClass:NSNumber.class]||![binding[@"arg_id"] isKindOfClass:NSNumber.class])return nil;
        int reg=[binding[@"reg"] intValue],aid=[binding[@"arg_id"] intValue];
        // c1 and driver uniforms are not dispatch-frozen by the arena contract.
        if(reg<0)continue;if(reg>=16||aid<0||aid>=NVN_REFL_MAXB)return nil;
        int found=0;
        for(int i=0;i<resolved;i++){const nvn_rbind *b=&sh->rf.b[i];if(b->kind==NVN_RB_CBUF&&b->reg==reg&&b->aid==aid){found=1;break;}}
        if(!found)continue;
        size_t size=S.ubo[stage][reg].size;if(!size||size>65536)continue;
        const uint8_t *bytes=bound_uniform_bytes(slots[aid],size,nil);if(!bytes)continue;
        CC_SHA256_Update(&digest,&reg,sizeof reg);CC_SHA256_Update(&digest,&aid,sizeof aid);CC_SHA256_Update(&digest,&size,sizeof size);CC_SHA256_Update(&digest,bytes,(CC_LONG)size);
        NSData *data=[NSData dataWithBytes:bytes length:size];
        [inputs addObject:@{@"name":binding[@"name"],@"reg":@(reg),@"arg_id":@(aid),@"immutable":@YES,@"data":[data base64EncodedStringWithOptions:0]}];
    }
    unsigned char hash[32];CC_SHA256_Final(hash,&digest);NSData *identity=[NSData dataWithBytes:hash length:32];
    if([sh->raw_write_attempted containsObject:identity])return nil;
    if(!sh->raw_write_attempted)sh->raw_write_attempted=[NSMutableSet set];[sh->raw_write_attempted addObject:identity];sh->raw_write_auto_attempts++;
    NSDictionary *request=@{@"abi":@1,@"spirv":cap[@"spirv"],@"grid":@[@(g_dispatch_grid[0]),@(g_dispatch_grid[1]),@(g_dispatch_grid[2])],
        @"local":@[@(sh->rf.tg[0]),@(sh->rf.tg[1]),@(sh->rf.tg[2])],@"simd_width":@32,@"bindings":inputs};
    /* The analysis takes 0.2-0.3 s (2026-11-10: 256-308 ms per proof in the game log, one long frame each when it ran
       here). It runs on a serial background queue; this dispatch falls back, and raw_write_auto_collect() adds the
       result on a later dispatch, where raw_write_domain_match accepts it against the bindings of that dispatch. */
    int attempt=sh->raw_write_auto_attempts;
    dispatch_async(raw_write_auto_queue(),^{
        uint64_t t=mach_absolute_time();NSDictionary *result=nil;
        NSData *json=[NSJSONSerialization dataWithJSONObject:request options:0 error:nil];
        if(json&&json.length<=4194304){
            NSMutableData *terminated=[json mutableCopy];uint8_t zero=0;[terminated appendBytes:&zero length:1];
            char *reply=calloc(1,131072);
            if(reply){
                int n=prove(terminated.bytes,reply,131072);
                if(n>0&&n<131072)result=[NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:reply length:(size_t)n] options:0 error:nil];
                free(reply);
            }
        }
        id done=[result isKindOfClass:NSDictionary.class]?result:(id)NSNull.null;
        os_unfair_lock_lock(&g_raw_write_auto_lock);
        if(!g_raw_write_auto_done)g_raw_write_auto_done=[NSMutableArray array];
        [g_raw_write_auto_done addObject:@[sh,done,@(attempt),@(mach_absolute_time()-t)]];
        os_unfair_lock_unlock(&g_raw_write_auto_lock);atomic_store(&g_raw_write_auto_ready,1);
    });
    return nil;
}
