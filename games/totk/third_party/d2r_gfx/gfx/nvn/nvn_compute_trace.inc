/* Exact-source, opt-in instrumentation of a real compute shader. The external
 * config fingerprints both the original MSL and its diagnostic variant.
 * Buffer 30 is a private trace output, never a game resource. */
static uint64_t zero_bank_gpu(void);
static NSDictionary *cs_trace_config(void) {
    static int init;static NSDictionary *config;
    if(!init){init=1;const char *path=getenv("OOT_CS_TRACE_CONFIG");if(path&&*path){NSData *data=[NSData dataWithContentsOfFile:@(path)];
        config=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        if(![config isKindOfClass:NSDictionary.class])MDIE("invalid compute trace config");}}
    return config;
}
static NSString *cs_trace_sha(NSString *source) {
    NSData *data=[source dataUsingEncoding:NSUTF8StringEncoding];unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString *hex=[NSMutableString string];for(unsigned i=0;i<sizeof digest;i++)[hex appendFormat:@"%02x",digest[i]];return hex;
}
static int cs_trace_match(NvShader *shader) {
    NSDictionary *config=cs_trace_config();NSString *prefix=config[@"prefix"];
    return prefix.length&&shader&&!strncmp(shader->key,prefix.UTF8String,prefix.length);
}
static void cs_trace_function(NvShader *shader) {
    if(!cs_trace_match(shader))return;
    static NSMutableSet *done;if(!done)done=[NSMutableSet set];NSValue *key=[NSValue valueWithPointer:(__bridge void*)shader];if([done containsObject:key])return;[done addObject:key];
    NSDictionary *config=cs_trace_config();NSString *original=sh_src(shader),*source=[NSString stringWithContentsOfFile:config[@"source"]encoding:NSUTF8StringEncoding error:nil];
    if(!original||![cs_trace_sha(original)isEqual:config[@"original_sha256"]]||!source||![cs_trace_sha(source)isEqual:config[@"instrumented_sha256"]])MDIE("compute trace source fingerprint mismatch: %s",shader->key);
    NSError *error=nil;id<MTLLibrary> library=totk_msl_library(source,&error);id<MTLFunction> function=library?[library newFunctionWithName:shader->fn.name]:nil;
    if(!function)MDIE("compute trace compile: %s",error.localizedDescription.UTF8String);
    shader->fn=function;fn_note_src(function,source);MLOG("[cs-trace] exact-source instrumentation enabled for %s",shader->key);
}
static void cs_trace_prepare(NvShader *shader) {
    if(!cs_trace_match(shader))return;
    static NSMutableSet *done;if(!done)done=[NSMutableSet set];uint64_t frame=atomic_load(&g_cur_frame);NSString *key=[NSString stringWithFormat:@"%s-F%llu",shader->key,frame];
    [g_ct setAddress:zero_bank_gpu()atIndex:30];if(!passdump()||[done containsObject:key])return;[done addObject:key];
    id<MTLBuffer> buffer=[g_dev newBufferWithLength:512 options:MTLResourceStorageModeShared];if(!buffer)MDIE("compute trace buffer allocation");res_add(buffer);
    memset(buffer.contents,0,512);((uint32_t*)buffer.contents)[0]=0x4f4f5452;[g_ct setAddress:buffer.gpuAddress atIndex:30];
    NSString *directory=cs_trace_config()[@"directory"],*path=[directory stringByAppendingPathComponent:[key stringByAppendingString:@".bin"]];
    [g_commit_blocks addObject:^{BOOL ok=[[NSData dataWithBytes:buffer.contents length:512]writeToFile:path atomically:YES];MLOG("[cs-trace] GPU %s %s ready=%u",ok?"saved":"FAILED",path.UTF8String,((uint32_t*)buffer.contents)[1]);}];defer_obj_release(buffer);
}
