/* Optional translations emitted by our typed compiler. Exact original and
 * candidate source/reflection fingerprints plus identical bindings are required.
 * Mode 1: complete primitive-block allocation. Mode 2: allocation + raw spans.
 * External generated game shaders remain outside the source checkout. */
static NSMutableDictionary *g_mesh_translation_info;
static uint64_t g_mesh_translation_hits[3],g_mesh_translation_loaded[3];
static int mtk_translation_mode(void) {
    int v=atomic_load(&g_mesh_translation);
    if(v<0){const char*s=getenv("OOT_MESH_TRANSLATION");v=s?atoi(s):0;atomic_store(&g_mesh_translation,v);}
    return !TARGET_OS_IPHONE&&v>=1&&v<=2?v:0;
}
static NSString *mtk_translation_sha(NSData*d) {
    if(!d)return nil;unsigned char h[32];char hex[65]={0};CC_SHA256(d.bytes,(CC_LONG)d.length,h);
    for(int i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",h[i]);return @(hex);
}
static id<MTLFunction> mtk_translation_function(NvShader*ms,int *selected) {
    int mode=mtk_translation_mode();*selected=0;
    if(!mode||!ms||ms->mesh_kind!=2)return ms->fn;
    const char*dir=getenv("OOT_MESH_TRANSLATION_DIR");if(!dir)return ms->fn;
    if(!g_mesh_translation_info)g_mesh_translation_info=[NSMutableDictionary dictionary];
    NSString*cacheKey=[NSString stringWithFormat:@"%d:%s",mode,ms->key];
    id saved=g_mesh_translation_info[cacheKey];
    if(saved){if(saved==NSNull.null)return ms->fn;*selected=mode;return saved;}
    g_mesh_translation_info[cacheKey]=NSNull.null;
    NSString*stem=[NSString stringWithFormat:@"%s/%s/%s",dir,mode==1?"bounds":"span",ms->key];
    NSData*proof=[NSData dataWithContentsOfFile:[stem stringByAppendingString:@".variant.json"]];
    NSDictionary*j=proof&&proof.length<32768?[NSJSONSerialization JSONObjectWithData:proof options:0 error:nil]:nil;
    if(![j isKindOfClass:NSDictionary.class]||[j[@"abi"]intValue]!=1||![j[@"key"]isEqual:@(ms->key)]||
       ![j[@"kind"]isEqual:mode==1?@"mesh_bounds":@"raw_span"])return ms->fn;
    NSData*src=[NSData dataWithContentsOfFile:[stem stringByAppendingString:@".metal"]];
    NSData*ref=[NSData dataWithContentsOfFile:[stem stringByAppendingString:@".json"]];
    NSData*original=[sh_src(ms)dataUsingEncoding:NSUTF8StringEncoding];
    NSData*oref=totk_rt_read([[ms->src_path stringByDeletingPathExtension]stringByAppendingPathExtension:@"json"]);
    if(![j[@"original_source_sha256"]isEqual:mtk_translation_sha(original)]||
       ![j[@"original_reflection_sha256"]isEqual:mtk_translation_sha(oref)]||
       ![j[@"candidate_source_sha256"]isEqual:mtk_translation_sha(src)]||
       ![j[@"candidate_reflection_sha256"]isEqual:mtk_translation_sha(ref)]){
        MLOG("[mesh-translation] hash fallback mode=%d %.16s",mode,ms->key);return ms->fn;
    }
    NSMutableDictionary*r=ref?[NSJSONSerialization JSONObjectWithData:ref options:NSJSONReadingMutableContainers error:nil]:nil;
    NSDictionary*o=oref?[NSJSONSerialization JSONObjectWithData:oref options:0 error:nil]:nil;
    if(![r isKindOfClass:NSMutableDictionary.class]||![o isKindOfClass:NSDictionary.class])return ms->fn;
    if(mode==2){if([r[@"raw_read_span_abi"]intValue]!=1)return ms->fn;[r removeObjectForKey:@"raw_read_span_abi"];}
    if(![r isEqual:o]){MLOG("[mesh-translation] binding fallback mode=%d %.16s",mode,ms->key);return ms->fn;}
    NSString*source=[[NSString alloc]initWithData:src encoding:NSUTF8StringEncoding];NSError*error=nil;
    id<MTLLibrary>lib=source?variant_library(source,&error):nil;id<MTLFunction>fn=lib?[lib newFunctionWithName:ms->fn.name]:nil;
    if(!fn){MLOG("[mesh-translation] compile fallback mode=%d %.16s %s",mode,ms->key,error.description.UTF8String);return ms->fn;}
    fn_note_src(fn,source);fn.label=[NSString stringWithFormat:@"%s [typed-%d]",ms->key,mode];
    g_mesh_translation_info[cacheKey]=fn;g_mesh_translation_loaded[mode]++;*selected=mode;
    MLOG("[mesh-translation] loaded mode=%d %.16s spans=%u words=%u",mode,ms->key,[j[@"span_mentions"]unsignedIntValue],[j[@"words"]unsignedIntValue]);return fn;
}
