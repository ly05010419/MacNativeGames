/* Selected compute resources, immediately before/after the actual dispatch.
 * GPU copies retain timeline order; files are read only at commit completion.
 * OOT_CS_RESOURCE_CAPTURE=<external directory>, OOT_CS_RESOURCE_CAPTURE_KEYS
 * selects shader prefixes. A runtime "passdump 1" arms one frame. */
static size_t cs_capture_pixel_bytes(MTLPixelFormat format) {
    switch(format) {
        case MTLPixelFormatR8Unorm:case MTLPixelFormatR8Uint:return 1;
        case MTLPixelFormatRG8Unorm:case MTLPixelFormatRG8Uint:case MTLPixelFormatR16Float:case MTLPixelFormatR16Uint:return 2;
        case MTLPixelFormatRGBA8Unorm:case MTLPixelFormatRGBA8Unorm_sRGB:case MTLPixelFormatBGRA8Unorm:case MTLPixelFormatRGBA8Uint:
        case MTLPixelFormatRG16Float:case MTLPixelFormatRG16Uint:case MTLPixelFormatRG16Unorm:case MTLPixelFormatR32Float:case MTLPixelFormatR32Uint:case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG11B10Float:case MTLPixelFormatRGB9E5Float:return 4;
        case MTLPixelFormatRGBA16Float:case MTLPixelFormatRGBA16Uint:case MTLPixelFormatRG32Float:case MTLPixelFormatRG32Uint:return 8;
        case MTLPixelFormatRGBA32Float:case MTLPixelFormatRGBA32Uint:return 16;
        default:return 0;
    }
}
static void cs_capture(NvShader *shader,const char *phase) {
    static int init;static NSString *directory;static const char *keys;static NSMutableSet *done;
    if(!init){init=1;const char *p=getenv("OOT_CS_RESOURCE_CAPTURE");keys=getenv("OOT_CS_RESOURCE_CAPTURE_KEYS");if(p&&*p&&keys&&*keys){directory=@(p);done=[NSMutableSet set];}}
    if(!directory||!passdump()||!diag_shader_prefix_csv_matches(shader->key,keys))return;
    uint64_t frame=atomic_load(&g_cur_frame);
    NSString *stem=[NSString stringWithFormat:@"F%llu-%.16s-%s",frame,shader->key,phase];
    if([done containsObject:stem])return;[done addObject:stem];
    NSMutableArray *records=[NSMutableArray array];
    for(int i=0;i<shader->rf.nb;i++) {
        const nvn_rbind *binding=&shader->rf.b[i];unsigned reg=binding->reg;
        if(binding->kind==NVN_RB_IMG||binding->kind==NVN_RB_TEX) {
            uint64_t handle=binding->kind==NVN_RB_IMG?(reg<32?S.img[5][reg]:0):(reg<128?S.tex[5][reg]:0);
            int tid=handle&0xfffff;registry_lock();
            if(tid<=0||tid>=g_ntreg||!g_treg[tid].ok){registry_unlock();continue;}
            void *object=NULL;if(binding->kind==NVN_RB_IMG)image_fast_rid(tid,&object);else tex_fast_rid(tid,&object);id<MTLTexture> texture=(__bridge id<MTLTexture>)object;
            tex_t guest=g_treg[tid].t;int view_format=g_treg[tid].has_v&&g_treg[tid].v.set_format?g_treg[tid].v.format:guest.d.format;
            if(!texture||texture.textureType!=MTLTextureType2D){registry_unlock();continue;}
            id<MTLTexture> parent=texture;while(parent.parentTexture)parent=parent.parentTexture;
            size_t pixel=cs_capture_pixel_bytes(texture.pixelFormat),row=(texture.width*pixel+255)&~(size_t)255,bytes=row*texture.height;
            if(!pixel||bytes>(32u<<20)){registry_unlock();MLOG("[cs-capture] unsupported format/extent %lu",(unsigned long)texture.pixelFormat);continue;}
            NSString *name=[NSString stringWithFormat:@"%@-%s%u.bin",stem,binding->kind==NVN_RB_IMG?"image":"texture",reg];
            NSMutableDictionary *record=[@{@"kind":binding->kind==NVN_RB_IMG?@"image":@"texture",@"slot":@(reg),@"descriptor":@(tid),@"serial":@(guest.serial),
                @"memory":[NSString stringWithFormat:@"%p",guest.mem],@"nvn_format":@(guest.d.format),@"view_format":@(view_format),@"texture_flags":@(guest.d.flags),@"pool_flags":@(guest.pool?guest.pool->flags:0),
                @"metal_format":@(texture.pixelFormat),@"width":@(texture.width),@"height":@(texture.height),@"bytes_per_pixel":@(pixel),@"bytes_per_row":@(row),
                @"resource_id":@(texture.gpuResourceID._impl),@"parent_resource_id":@(parent.gpuResourceID._impl),@"usage":@(texture.usage),@"parent_usage":@(parent.usage),@"parent_format":@(parent.pixelFormat),@"file":name}mutableCopy];
            registry_unlock();
            id<MTLBuffer> copy=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];if(!copy)MDIE("compute capture allocation");res_add(copy);
            enc_b_copy_texture_buffer(blit(),texture,0,0,MTLOriginMake(0,0,0),MTLSizeMake(texture.width,texture.height,1),copy,0,row,bytes);
            [records addObject:record];NSString *path=[directory stringByAppendingPathComponent:name];
            [g_commit_blocks addObject:^{BOOL ok=[[NSData dataWithBytes:copy.contents length:bytes]writeToFile:path atomically:YES];MLOG("[cs-capture] GPU %s %s",ok?"saved":"FAILED",path.UTF8String);}];defer_obj_release(copy);
        } else if(binding->kind==NVN_RB_SBUFP&&reg<16) {
            const p_bind_buf *bound=&S.ssbo[5][reg];size_t bytes=(size_t)bound->size;void *source;uint64_t offset,gpu;
            if(!bytes||bytes>(8u<<20)||!mtl_gpu_addr(bound->addr,bytes,&source,&offset,&gpu))continue;
            NSString *name=[NSString stringWithFormat:@"%@-storage%u.bin",stem,reg],*path=[directory stringByAppendingPathComponent:name];
            id<MTLBuffer> copy=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];if(!copy)MDIE("compute capture allocation");res_add(copy);
            enc_b_copy_buffer(blit_buffer((__bridge id<MTLBuffer>)source,offset,copy,0,bytes),(__bridge id<MTLBuffer>)source,offset,copy,0,bytes);
            [records addObject:@{@"kind":@"storage",@"slot":@(reg),@"address":@(bound->addr),@"bytes":@(bytes),@"file":name}];
            [g_commit_blocks addObject:^{[[NSData dataWithBytes:copy.contents length:bytes]writeToFile:path atomically:YES];}];defer_obj_release(copy);
        }
    }
    NSString *push=[stem stringByAppendingString:@"-push.bin"];
    [[NSData dataWithBytes:S.push[1] length:NVN_PUSH_SIZE]writeToFile:[directory stringByAppendingPathComponent:push]atomically:YES];
    NSDictionary *manifest=@{@"shader":@(shader->key),@"frame":@(frame),@"phase":@(phase),@"grid":@[@(g_dispatch_grid[0]),@(g_dispatch_grid[1]),@(g_dispatch_grid[2])],@"push":push,@"resources":records};
    [[NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".json"]]atomically:YES];
}
