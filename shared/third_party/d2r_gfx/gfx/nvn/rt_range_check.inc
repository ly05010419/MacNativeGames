// 私有原游戏检查器：同帧初态→候选→恢复初态→原双pass；仅验证已观测outline/billboard，不进普通构建。
// Private checker for the original game: same-frame initial state → candidate → restore initial state → original two passes; only verifies the observed outline/billboard, not part of normal builds.
typedef struct { typeof(S) s; typeof(R) r; tex_t c,d; p_draw dw; p_draw_idx di; int indexed; } rr_snapshot;
static rr_snapshot g_rr_s[2];
static int g_rr_active,g_rr_n,g_rr_bad,g_rr_first_fresh,g_rr_resets;
static id<MTLBuffer> g_rr_vis[2],g_rr_before,g_rr_before_vis;static int g_rr_slot[2],g_rr_before_slot,g_rr_use_before;
static void rr_reset(void){if(g_rr_active&&!g_rr_checking){g_rr_resets++;if(g_rr_n!=1)g_rr_bad=1;}}
static uint64_t g_rr_frame,g_rr_dirty,g_rr_load,g_rr_store,g_rr_reg,g_rr_ser;
static id<MTLTexture> g_rr_initial[2];
static tex_t g_rr_t[2];
static void rr_copy(id<MTLTexture> a,id<MTLTexture> b) {
    [blit()copyFromTexture:a sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(a.width,a.height,1)toTexture:b destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
}
static void rr_begin(void) {
    if(g_rr_checking||g_rr_active||!rt_range_on()||!getenv("DM_NVN_RT_RANGE_CHECK")||!passdump()||g_fz.active||!R.has_rt||R.rt.n!=1||!R.rt.depth||R.clear_c[0]||R.clear_d||R.clear_s)return;
    NvProg *p=prog_get(S.prog);if(!rt_range_pr_ro(p)||strcmp(p->sh[1]->key,"6d19a794f1206668_b")||!S.ds.dt||S.ds.dw||S.ds.st)return;
    p_set_rt b=R.rt;b.cview[0].nlevels=1;if(!rt_range_shape(&R.rt,&b)||R.rt.cview[0].base_level||R.rt.dview.base_level)return;
    id<MTLTexture> color=tex_mtl(R.rt.color[0]),depth=tex_mtl(R.rt.depth);
    if(color.pixelFormat!=MTLPixelFormatRG11B10Float||depth.pixelFormat!=MTLPixelFormatDepth32Float_Stencil8||color.width!=depth.width||color.height!=depth.height||color.sampleCount!=1||depth.sampleCount!=1)return;
    g_rr_t[0]=*R.rt.color[0];g_rr_t[1]=*R.rt.depth;
    for(int k=0;k<2;k++){id<MTLTexture> t=tex_mtl(&g_rr_t[k]);MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;g_rr_initial[k]=[g_dev newTextureWithDescriptor:d];if(!g_rr_initial[k])abort();rr_copy(t,g_rr_initial[k]);}
    g_rr_frame=atomic_load(&g_cur_frame);g_rr_dirty=nvn_dirty_now();g_rr_load=g_bw_load;g_rr_store=g_bw_store;g_rr_first_fresh=g_vis_cur<0;g_rr_before_vis=g_vis;g_rr_before_slot=g_vis_cur;g_rr_use_before=0;g_rr_before=[g_dev newBufferWithLength:8 options:MTLResourceStorageModeShared];
    if(g_vis_cur>=0 && g_vis)[blit()copyFromBuffer:g_vis sourceOffset:g_vis_cur*8 toBuffer:g_rr_before destinationOffset:0 size:8];g_rr_resets=0;g_rr_vis[0]=g_rr_vis[1]=nil;g_rr_n=g_rr_bad=0;g_rr_active=1;
}
static void rr_snap(const p_draw *dw,const p_draw_idx *di) {
    if(!g_rr_active||g_rr_checking)return;if(g_rr_n>=2){g_rr_bad=1;return;}int k=g_rr_n;if(!k){g_rr_dirty=nvn_dirty_now();g_rr_reg=g_reg_gen;g_rr_ser=g_ser_gen;}else if(g_rr_reg!=g_reg_gen||g_rr_ser!=g_ser_gen)g_rr_bad=1;if(!k)g_rr_use_before=g_vis==g_rr_before_vis && g_vis_cur==g_rr_before_slot && g_vis_cur>=0;g_rr_vis[k]=(g_vis_on&&g_vis_counting&&g_vis_cur>=0)?g_vis:nil;g_rr_slot[k]=g_vis_cur;rr_snapshot *z=&g_rr_s[g_rr_n++];memcpy(&z->s,&S,sizeof S);z->r=R;z->c=*R.rt.color[0];z->d=*R.rt.depth;z->r.rt.color[0]=&z->c;z->r.rt.depth=&z->d;z->indexed=di!=NULL;if(dw)z->dw=*dw;if(di)z->di=*di;
}
static NSArray *rr_read(id<MTLTexture> c,id<MTLTexture> d) {
    NSUInteger w=c.width,h=c.height;id<MTLBuffer> a=[g_dev newBufferWithLength:w*h*4 options:MTLResourceStorageModeShared],b=[g_dev newBufferWithLength:w*h*4 options:MTLResourceStorageModeShared],e=[g_dev newBufferWithLength:w*h options:MTLResourceStorageModeShared];
    [blit()copyFromTexture:c sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(w,h,1)toBuffer:a destinationOffset:0 destinationBytesPerRow:w*4 destinationBytesPerImage:w*h*4];
    [blit()copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(w,h,1)toBuffer:b destinationOffset:0 destinationBytesPerRow:w*4 destinationBytesPerImage:w*h*4 options:MTLBlitOptionDepthFromDepthStencil];
    [blit()copyFromTexture:d sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)sourceSize:MTLSizeMake(w,h,1)toBuffer:e destinationOffset:0 destinationBytesPerRow:w destinationBytesPerImage:w*h options:MTLBlitOptionStencilFromDepthStencil];return @[a,b,e];
}
static void rr_finish(int ranged) {
    if(!g_rr_active||g_rr_checking)return;g_rr_active=0;
    if(!ranged||g_rr_n!=2||g_rr_bad||g_rr_dirty!=nvn_dirty_now() || g_rr_reg!=g_reg_gen || g_rr_ser!=g_ser_gen || g_rr_resets!=1 || !g_rr_vis[0] || !g_rr_vis[1] || !g_rr_s[0].s.ds.dt || g_rr_s[0].s.ds.dw || g_rr_s[0].s.ds.st || !g_rr_s[1].s.ds.dt || g_rr_s[1].s.ds.dw || g_rr_s[1].s.ds.st){NSString *p=[@(getenv("DM_NVN_RT_RANGE_CHECK")) stringByAppendingPathComponent:@"SKIP.tsv"];FILE *f=fopen(p.UTF8String,"a");if(f){fprintf(f,"%llu\t%d\t%d\t%d\t%d\t%d\t%d\t%d\n",(unsigned long long)g_rr_frame,ranged,g_rr_n,g_rr_bad,g_rr_dirty!=nvn_dirty_now(),g_rr_resets,g_rr_vis[0]!=nil,g_rr_vis[1]!=nil);fclose(f);}g_rr_initial[0]=g_rr_initial[1]=nil;return;}
    uint64_t candidate_bytes=g_bw_load-g_rr_load+g_bw_store-g_rr_store;
    g_rr_checking=1;typeof(S) ss;memcpy(&ss,&S,sizeof S);typeof(R) rs=R;const uint8_t *next=g_exec_next,*cur_op=g_exec_cur;
    uint64_t curpass=g_vis_cur_pass;int vis=g_vis_on,counting=g_vis_counting,cur=g_vis_cur,visn=g_vis_n,vsn=g_nvs,vscap=g_vscap;id<MTLBuffer> oldvis=g_vis;vis_slot *oldvs=g_vs;
    id<MTLBuffer> before=g_rr_before;int use_before=g_rr_use_before;NSArray *actualvis=@[g_rr_vis[0],g_rr_vis[1]];int slot0=g_rr_slot[0],slot1=g_rr_slot[1];uint64_t pass=g_stat_pass,draws=g_stat_draw,load=g_bw_load,store=g_bw_store,full=g_pass_full_rw,dead=g_sd_dead_bytes,ndead=g_sd_n_dead,nall=g_sd_n_all;
    id<MTLTexture> c=tex_mtl(&g_rr_t[0]),d=tex_mtl(&g_rr_t[1]);id<MTLTexture> saved[2];id<MTLTexture> source[2]={c,d};
    for(int k=0;k<2;k++){id<MTLTexture> t=source[k];MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;saved[k]=[g_dev newTextureWithDescriptor:td];if(!saved[k])abort();rr_copy(t,saved[k]);}
    if(g_rr_reg!=g_reg_gen||g_rr_ser!=g_ser_gen)MDIE("RT范围oracle资源代号变化，提交前停止未比");
    NSArray *candidate=rr_read(c,d);rr_copy(g_rr_initial[0],c);rr_copy(g_rr_initial[1],d);g_vis=[g_dev newBufferWithLength:VIS_SLOTS*8 options:MTLResourceStorageModeShared];g_vis_n=0;g_vis_cur=-1;g_vis_on=g_vis_counting=1;g_vs=NULL;g_nvs=g_vscap=0;g_exec_next=g_exec_cur=NULL;
    id<MTLBuffer> refvis[2];int refslot[2];
    for(int i=0;i<2;i++){if(i)rp_end();g_vis_cur=-1;memcpy(&S,&g_rr_s[i].s,sizeof S);R=g_rr_s[i].r;g_pso_dirty=g_dss_dirty=1;rp_begin_ex(4); // 原trace两个pass均mode4，不以无扫描起点的mode0多挂模板 | In the original trace both passes are mode4; do not attach an extra stencil with mode0, which has no scan start point
        draw(g_rr_s[i].indexed?NULL:&g_rr_s[i].dw,g_rr_s[i].indexed?&g_rr_s[i].di:NULL);refvis[i]=g_vis;refslot[i]=g_vis_cur;}rp_end();
    uint64_t original_passes=g_stat_pass-pass,original_bytes=g_bw_load-load+g_bw_store-store;NSArray *reference=rr_read(c,d);
    if(g_rr_reg!=g_reg_gen||g_rr_ser!=g_ser_gen||g_rr_dirty!=nvn_dirty_now())MDIE("RT范围oracle绑定期间资源/数据代号变化，提交前停止未比");
    rr_copy(saved[0],c);rr_copy(saved[1],d); // 后继命令读取candidate，不能由oracle覆盖凑截图绿 | Subsequent commands read the candidate; the oracle must not overwrite it to force the screenshot green
    NSArray *referencevis=@[refvis[0],refvis[1]];int refslot0=refslot[0],refslot1=refslot[1];vis_clear();free(g_vs);g_vs=oldvs;g_nvs=vsn;g_vscap=vscap;g_vis=oldvis;g_vis_n=visn;
    memcpy(&S,&ss,sizeof S);R=rs;g_exec_next=next;g_exec_cur=cur_op;g_vis_on=vis;g_vis_counting=counting;g_vis_cur=cur;g_vis_cur_pass=curpass;g_pso_dirty=g_dss_dirty=1;g_stat_pass=pass;g_stat_draw=draws;g_bw_load=load;g_bw_store=store;g_pass_full_rw=full;g_sd_dead_bytes=dead;g_sd_n_dead=ndead;g_sd_n_all=nall;
    uint64_t frame=g_rr_frame;unsigned vc=g_rr_s[1].indexed?g_rr_s[1].di.count:g_rr_s[1].dw.count;NSUInteger n=c.width*c.height;NSString *out=@(getenv("DM_NVN_RT_RANGE_CHECK"));
    [g_cb addCompletedHandler:^(id<MTLCommandBuffer> cb){unsigned long long diff[3]={0};
        unsigned long long av[2],rv[2];int as[2]={slot0,slot1},rs[2]={refslot0,refslot1};for(int k=0;k<2;k++){id<MTLBuffer> a=actualvis[k],r=referencevis[k];av[k]=((uint64_t *)a.contents)[as[k]];rv[k]=((uint64_t *)r.contents)[rs[k]];}if(use_before)av[0]-=*(uint64_t *)before.contents;const char *names[3]={"color","depth","stencil"};
        for(int k=0;k<3;k++){id<MTLBuffer> a=candidate[k],b=reference[k];size_t bp=k==2?1:4;for(NSUInteger j=0;j<n;j++)diff[k]+=memcmp((uint8_t *)a.contents+j*bp,(uint8_t *)b.contents+j*bp,bp)!=0;
            for(int m=0;m<2;m++){id<MTLBuffer> x=m?b:a;NSString *path=[NSString stringWithFormat:@"%@/F%llu_P%llu_%s_%s.raw",out,(unsigned long long)frame,(unsigned long long)pass,m?"reference":"candidate",names[k]];FILE *f=fopen(path.UTF8String,"wb");if(!f||fwrite(x.contents,1,n*bp,f)!=n*bp)MLOG("【红】RT范围检查raw写盘失败 %s",path.UTF8String);if(f)fclose(f);}}
        NSString *qp=[out stringByAppendingPathComponent:@"QUERY_RESULTS.tsv"];FILE *qf=fopen(qp.UTF8String,"a");if(qf){fprintf(qf,"%llu\t%llu\t%llu\t%llu\t%llu\t%llu\n",(unsigned long long)frame,(unsigned long long)pass,rv[0],rv[1],av[0],av[1]);fclose(qf);}else MLOG("【红】RT范围query写盘失败");
        NSString *result=[out stringByAppendingPathComponent:@"CHECK_RESULTS.tsv"];FILE *rf=fopen(result.UTF8String,"a");if(rf){fprintf(rf,"%llu\t%llu\t%u\t%llu\t1\t%llu\t%llu\t%llu\t%llu\t%llu\t%lu\t%lu\n",(unsigned long long)frame,(unsigned long long)pass,vc,(unsigned long long)original_passes,(unsigned long long)original_bytes,(unsigned long long)candidate_bytes,diff[0],diff[1],diff[2],(unsigned long)n,(unsigned long)cb.status);fclose(rf);}else MLOG("【红】RT范围结果写盘失败");
        MLOG("%sRT范围query F%llu P%llu 原=%llu,%llu 候选=%llu,%llu",av[0]!=rv[0]||av[1]!=rv[1]?"【红】":"",(unsigned long long)frame,(unsigned long long)pass,rv[0],rv[1],av[0],av[1]);
        MLOG("%sRT范围双渲染 F%llu P%llu vc=%u 原pass=%llu 候选pass=1 原模型=%llu 候选模型=%llu diff=%llu,%llu,%llu/%lu CB=%lu",diff[0]||diff[1]||diff[2]?"【红】":"",(unsigned long long)frame,(unsigned long long)pass,vc,(unsigned long long)original_passes,(unsigned long long)original_bytes,(unsigned long long)candidate_bytes,diff[0],diff[1],diff[2],(unsigned long)n,(unsigned long)cb.status);
    }];g_rr_initial[0]=g_rr_initial[1]=nil;g_rr_checking=0;
}
