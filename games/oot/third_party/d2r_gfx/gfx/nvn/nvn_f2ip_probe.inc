/* Runs independently generated SASS -> SPIR-V -> MSL fixtures through the
   production Metal 4 dispatch and buffer bindings. Expected values come from
   tests/f2ip_gpu.py's CPU rounding/saturation/packing oracle. */
int mtl_probe_f2ip(void) {
    const char *path=getenv("OOT_F2IP_CASES");if(!path)MDIE("OOT_F2IP_CASES is required");
    NSData *data=[NSData dataWithContentsOfFile:@(path)];NSError *error=nil;
    NSArray *cases=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
    if(![cases isKindOfClass:NSArray.class]||!cases.count)MDIE("invalid F2IP fixture manifest");
    mtl_init();void *memory=NULL;if(posix_memalign(&memory,16384,16384))MDIE("F2IP probe allocation");
    memset(memory,0,16384);mtl_pool_add(memory,16384);int checked=0;
    unsigned raw_count=0;
    for(NSDictionary *fixture in cases)raw_count=MAX(raw_count,[fixture[@"raw_pool_count"]unsignedIntValue]);
    if(raw_count>16384)MDIE("raw pool fixture too large");
    size_t raw_bytes=(size_t)raw_count*16384;uint8_t *raw_pools=NULL;
    if(raw_count){
        raw_pools=mmap(NULL,raw_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
        if(raw_pools==MAP_FAILED)MDIE("raw pool fixture allocation");
        for(unsigned i=0;i<raw_count;i++)mtl_pool_add(raw_pools+(size_t)i*16384,4096);
    }
    NSMutableArray *shaders=[NSMutableArray array];prog_t program={.serial=0xfffffc01};
    for(NSDictionary *fixture in cases){@autoreleasepool{
        NSString *source=[NSString stringWithContentsOfFile:fixture[@"source"] encoding:NSUTF8StringEncoding error:&error];
        NSData *raw=[NSData dataWithContentsOfFile:fixture[@"reflection"]];
        NSDictionary *reflection=raw?[NSJSONSerialization JSONObjectWithData:raw options:0 error:&error]:nil;
        id<MTLLibrary> library=source?[g_dev newLibraryWithSource:source options:totk_precise_opts() error:&error]:nil;
        if(!library||!reflection)MDIE("F2IP fixture compile: %s",error.localizedDescription.UTF8String);
        NvShader *cs=[NvShader new];cs->fn=[library newFunctionWithName:@"main0"];cs->stage=5;cs->src=source;
        strlcpy(cs->key,[[NSString stringWithFormat:@"probe-f2ip-%d",checked]UTF8String],sizeof cs->key);
        char message[256];if(nvn_refl_from_json(reflection,&cs->rf,message,sizeof message))MDIE("F2IP reflection: %s",message);
        if(cs->rf.has_c1)c1_store(cs,source);
        cs->ae=[cs->fn newArgumentEncoderWithBufferIndex:0];cs->arg_len=cs->ae.encodedLength;drv_scan(cs,source);
        [shaders addObject:cs];NvProg *pr=[NvProg new];pr->sh[5]=cs;serial_put(program.serial,pr);
        if(fixture[@"raw_pool_index"]){
            unsigned idx=[fixture[@"raw_pool_index"]unsignedIntValue];if(idx>=raw_count)MDIE("raw fixture index");
            uint8_t *target=raw_pools+(size_t)idx*16384+([fixture[@"raw_unmapped"]boolValue]?8192:128);
            *(uint32_t *)target=0x12345678;((uint64_t *)memory)[1]=(uintptr_t)target;
        }
        *(uint32_t *)memory=0xdeadbeef;op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        S.prog=S.cprog=&program;S.ssbo[5][0]=(p_bind_buf){5,0,(uint64_t)memory,16384};
        p_dispatch dp={.x=1,.y=1,.z=1};dispatch(&dp);mtl_sub_commit();mtl_finish();
        uint32_t actual=*(uint32_t *)memory,expected=[fixture[@"expected"]unsignedIntValue];
        if(actual!=expected)MDIE("F2IP %s: GPU %#x expected %#x",[fixture[@"label"]UTF8String],actual,expected);
        checked++;
    }}
    S.prog=S.cprog=NULL;memset(S.ssbo,0,sizeof S.ssbo);serial_del(program.serial);mtl_pool_del(memory);free(memory);
    for(unsigned i=0;i<raw_count;i++)mtl_pool_del(raw_pools+(size_t)i*16384);
    if(raw_pools)munmap(raw_pools,raw_bytes);
    return checked;
}
