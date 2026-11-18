/* Independent multi-result image load: input coordinates remain live when
 * another result is dead. Original generated SASS is supplied by the fixture. */
static int multi_result_probe_case(NSDictionary *fixture,unsigned number,NSMutableArray *shaders) {
    NSError *error=nil;
    NSString *source=[NSString stringWithContentsOfFile:fixture[@"source"] encoding:NSUTF8StringEncoding error:&error];
    NSData *json=[NSData dataWithContentsOfFile:fixture[@"reflection"]];
    NSDictionary *reflection=json?[NSJSONSerialization JSONObjectWithData:json options:0 error:&error]:nil;
    id<MTLLibrary> library=source?totk_msl_library(source,&error):nil;
    if(!library||!reflection)MDIE("multi-result fixture compile: %s",error.localizedDescription.UTF8String);
    NvShader *cs=[NvShader new];cs->src=source;cs->fn=[library newFunctionWithName:@"main0"];cs->stage=5;
    snprintf(cs->key,sizeof cs->key,"probe-multi-result-%u",number);
    char message[256];if(nvn_refl_from_json(reflection,&cs->rf,message,sizeof message))MDIE("multi-result reflection: %s",message);
    if(cs->rf.has_c1)c1_store(cs,source);cs->ae=[cs->fn newArgumentEncoderWithBufferIndex:0];cs->arg_len=cs->ae.encodedLength;drv_scan(cs,source);
    // Production keeps NvShader objects alive in g_shcache; g_cps uses their
    // address as identity. Match that ownership for this entire fixture batch.
    [shaders addObject:cs];
    prog_t program={.serial=0xfffffa01};NvProg *pr=[NvProg new];pr->sh[5]=cs;serial_put(program.serial,pr);
    uint8_t *memory=NULL;if(posix_memalign((void **)&memory,16384,49152))MDIE("multi-result allocation");
    memset(memory,0,49152);mtl_pool_add(memory,49152);
    ((uint32_t *)memory)[2]=[fixture[@"coord"][0]unsignedIntValue];
    ((uint32_t *)memory)[3]=[fixture[@"coord"][1]unsignedIntValue];
    tex_t texture[2]={0};
    for(int i=0;i<2;i++){
        tex_t *t=&texture[i];t->serial=0xffffe000+number*2+i;t->mem=memory+(i+1)*16384;
        t->d.width=t->d.height=4;t->d.depth=t->d.levels=1;t->d.target=1;
        t->d.format=i?NVN_FORMAT_R32F:NVN_FORMAT_RGBA32F;t->d.flags=NVN_TEXTURE_FLAGS_IMAGE;
        t->d.swizzle_r=2;t->d.swizzle_g=3;t->d.swizzle_b=4;t->d.swizzle_a=5;nvn_tex_layout(&t->d,&t->L);
        if(t->L.size>16384)MDIE("multi-result image allocation too small");mtl_reg_tex(i+1,t,NULL);
    }
    id<MTLBuffer> readback=probe_buf(64);op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    // Populate the GPU image through the production buffer-to-texture path.
    // Floats match the formatted SULD/SUST declarations, isolating DCE.
    for(unsigned i=0;i<64;i++)((uint32_t *)(memory+256))[i]=0x3f800000+i;
    p_copy_b2t copy={.src=(uintptr_t)memory+256,.tex=&texture[0]};
    copy.r.width=copy.r.height=4;copy.r.depth=1;copy_b2t(&copy);
    id<MTLBuffer> input_readback=probe_buf(256);
    registry_lock();id<MTLTexture> input_image=tex_by_id(1);registry_unlock();
    if(!input_image)MDIE("multi-result input missing");
    enc_b_copy_texture_buffer(blit(),input_image,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),input_readback,0,64,256);
    p_clear_tex clear={0};clear.tex=&texture[1];clear.mask=15;
    clear.r.width=clear.r.height=4;clear.r.depth=1;clear.c.f[0]=-1.0f;clear_tex(&clear);
    S.prog=S.cprog=&program;S.img[5][0]=1;S.img[5][1]=2;
    S.ssbo[5][0]=(p_bind_buf){5,0,(uintptr_t)memory,16384};g_args_dirty[5]=1;
    p_dispatch args={.x=1,.y=1,.z=1};dispatch(&args);
    registry_lock();id<MTLTexture> image=tex_by_id(2);registry_unlock();if(!image)MDIE("multi-result output missing");
    enc_b_copy_texture_buffer(blit(),image,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),readback,0,16,64);
    mtl_sub_commit();mtl_finish();
    for(unsigned i=0;i<64;i++)if(((uint32_t *)input_readback.contents)[i]!=0x3f800000+i)
        MDIE("multi-result input word %u: %#x != %#x",i,((uint32_t *)input_readback.contents)[i],0x3f800000+i);
    const uint32_t *pixels=readback.contents;
    for(int i=0;i<16;i++){
        uint32_t want=i?0xbf800000:[fixture[@"expected"]unsignedIntValue];
        if(pixels[i]!=want)MDIE("multi-result %s pixel %d: %#x != %#x",[fixture[@"label"]UTF8String],i,pixels[i],want);
    }
    MLOG("multi-result passed: %s (64 input words, 16 output pixels)",[fixture[@"label"]UTF8String]);
    S.prog=S.cprog=NULL;memset(S.img,0,sizeof S.img);memset(S.ssbo,0,sizeof S.ssbo);
    for(int i=0;i<2;i++){mtl_reg_tex(i+1,NULL,NULL);serial_del(texture[i].serial);}
    serial_del(program.serial);stor_drop_range((uintptr_t)memory,(uintptr_t)memory+49152);
    mtl_pool_del(memory);free(memory);defer_obj_release(readback);defer_obj_release(input_readback);return 80;
}

/* Original-independent SASS fixtures exercise native image atomics through the
 * same reflection, image preparation and Metal 4 dispatch used by the game. */
int mtl_probe_sured(void) {
    const char *path=getenv("OOT_SURED_CASES");if(!path)MDIE("OOT_SURED_CASES is required");
    NSError *error=nil;NSData *raw=[NSData dataWithContentsOfFile:@(path)];NSArray *cases=raw?[NSJSONSerialization JSONObjectWithData:raw options:0 error:&error]:nil;
    if(![cases isKindOfClass:NSArray.class]||!cases.count)MDIE("invalid surface reduction fixtures");
    mtl_init();int checked=0;NSMutableArray *shaders=[NSMutableArray array];prog_t program={.serial=0xfffffb01};
    for(NSDictionary *fixture in cases){@autoreleasepool{
        if([fixture[@"multi_result"]boolValue]){checked+=multi_result_probe_case(fixture,(unsigned)checked,shaders);continue;}
        NSString *source=[NSString stringWithContentsOfFile:fixture[@"source"] encoding:NSUTF8StringEncoding error:&error];
        NSData *json=[NSData dataWithContentsOfFile:fixture[@"reflection"]];NSDictionary *reflection=json?[NSJSONSerialization JSONObjectWithData:json options:0 error:&error]:nil;
        id<MTLLibrary> library=source?totk_msl_library(source,&error):nil;if(!library||!reflection)MDIE("surface fixture compile: %s",error.localizedDescription.UTF8String);
        NvShader *cs=[NvShader new];cs->src=source;cs->fn=[library newFunctionWithName:@"main0"];cs->stage=5;snprintf(cs->key,sizeof cs->key,"probe-sured-%d",checked);
        char message[256];if(nvn_refl_from_json(reflection,&cs->rf,message,sizeof message))MDIE("surface reflection: %s",message);
        if(cs->rf.has_c1)c1_store(cs,source);cs->ae=[cs->fn newArgumentEncoderWithBufferIndex:0];cs->arg_len=cs->ae.encodedLength;drv_scan(cs,source);[shaders addObject:cs];
        NvProg *pr=[NvProg new];pr->sh[5]=cs;serial_put(program.serial,pr);
        void *memory=NULL;if(posix_memalign(&memory,16384,16384))MDIE("surface fixture allocation");
        uint32_t initial=[fixture[@"initial"]unsignedIntValue];for(int i=0;i<4096;i++)((uint32_t *)memory)[i]=initial;
        mtl_pool_add(memory,16384);tex_t texture={0};texture.serial=0xfffff800u+(unsigned)checked;texture.mem=memory;
        BOOL rg16=[fixture[@"format"]isEqual:@"rg16ui"];
        texture.d.width=texture.d.height=4;texture.d.depth=texture.d.levels=1;texture.d.target=1;texture.d.format=rg16?NVN_FORMAT_RG16UI:([fixture[@"signed"]boolValue]?NVN_FORMAT_R32I:NVN_FORMAT_R32UI);texture.d.flags=NVN_TEXTURE_FLAGS_IMAGE;
        texture.d.swizzle_r=2;texture.d.swizzle_g=3;texture.d.swizzle_b=4;texture.d.swizzle_a=5;nvn_tex_layout(&texture.d,&texture.L);
        if(texture.L.size>16384)MDIE("surface fixture backing allocation too small");
        mtl_reg_tex(1,&texture,NULL);id<MTLBuffer> readback=probe_buf(64);op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        // A real GPU producer establishes the initial image contents. These
        // fixtures deliberately have no CPU-loaded texture pool descriptor.
        p_clear_tex clear={0};clear.tex=&texture;clear.is_uint=1;clear.mask=15;
        clear.r.width=clear.r.height=4;clear.r.depth=1;clear.c.u[0]=rg16?initial&65535:initial;if(rg16)clear.c.u[1]=initial>>16;clear_tex(&clear);
        unsigned slot=fixture[@"slot"]?[fixture[@"slot"]unsignedIntValue]:0;if(slot>=32)MDIE("surface fixture slot out of range");
        S.prog=S.cprog=&program;S.img[5][slot]=1;g_args_dirty[5]=1;p_dispatch dispatch_args={.x=1,.y=1,.z=1};dispatch(&dispatch_args);
        registry_lock();id<MTLTexture> image=tex_by_id(1);registry_unlock();if(!image)MDIE("surface fixture image missing");
        enc_b_copy_texture_buffer(blit(),image,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),readback,0,16,64);mtl_sub_commit();mtl_finish();
        int target=[fixture[@"target"]intValue];uint32_t expected=[fixture[@"expected"]unsignedIntValue];const uint32_t *pixels=readback.contents;
        for(int i=0;i<16;i++){uint32_t want=i==target?expected:initial;if(pixels[i]!=want)MDIE("surface %s pixel %d: %#x != %#x",[fixture[@"label"]UTF8String],i,pixels[i],want);checked++;}
        S.prog=S.cprog=NULL;memset(S.img,0,sizeof S.img);mtl_reg_tex(1,NULL,NULL);serial_del(texture.serial);stor_drop_range((uintptr_t)memory,(uintptr_t)memory+16384);mtl_pool_del(memory);free(memory);defer_obj_release(readback);
    }}
    serial_del(program.serial);return checked;
}
