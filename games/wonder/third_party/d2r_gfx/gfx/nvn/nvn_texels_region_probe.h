/* Production ReadTexels/WriteTexels roundtrip, including the guest pool and Metal mirror. */
int mtl_probe_texels_region(void) {
    mtl_init();int checked=0;
    const int formats[]={0x25,0x19,0x1d,0x22,0x3a,0x3b,0x3c,0x41,0x48,0x79};
    for(int c=0;c<14;c++) {@autoreleasepool {
        tex_t t={0};t.serial=0xffffd000u+c;
        t.d=(nvn_texdesc){.width=2048,.height=1024,.depth=1,.levels=2,.target=1,.format=c<10?formats[c]:0x25,.samples=1};
        unsigned lv=0;
        if(c==10){t.d.target=2;t.d.width=1024;t.d.height=512;t.d.depth=4;lv=1;}
        if(c==11){t.d.target=4;t.d.depth=3;lv=1;}
        if(c==12){t.d.target=8;t.d.width=t.d.height=1024;t.d.levels=1;}
        if(c==13){t.d.width=t.d.height=128;t.d.levels=1;}
        nvn_tex_layout(&t.d,&t.L);size_t size=(t.L.size+getpagesize()-1)&~((size_t)getpagesize()-1);
        uint8_t *mem=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0),*expected=malloc(size);
        if(mem==MAP_FAILED||!expected)MDIE("texels-region probe allocation");
        for(size_t i=0;i<size;i++)mem[i]=(uint8_t)(i*13+(i>>9)+c);memcpy(expected,mem,size);
        pool_t pool={0};pool.mem=mem;pool.size=size;pool.flags=0x24;t.pool=&pool;t.mem=mem;
        mtl_pool_add(mem,size);nvn_dirty_add_range(mem,size);
        int bw,bh,bpb=nvn_fmt_bpb(t.d.format,&bw,&bh),is3d=t.d.target==2;
        NVNcopyRegion r={.xoffset=17*bw,.yoffset=19*bh,.zoffset=c==11?1:c==12?2:0,.width=11*bw,.height=7*bh,.depth=c>=10&&c<=12?2:1};
        size_t wb=11*bpb,hb=7,row=wb+13,img=row*hb+29,n=img*r.depth+17;
        uint8_t *got=malloc(n),*want=malloc(n),*src=malloc(n);
        if(!got||!want||!src)MDIE("texels-region probe CPU buffers");memset(got,0xa7,n);memset(want,0xa7,n);
        for(int k=0;k<r.depth;k++){
            unsigned layer=is3d?0:r.zoffset+k;uint32_t rb,rs,zs,bpb_;uint64_t base;unsigned th,td;
            if(nvn_deswizzle_geom(&t.d,lv,layer,&rb,&rs,&zs,&base,&th,&td,&bpb_))MDIE("texels-region probe geometry");
            for(size_t j=0;j<hb;j++)for(size_t i=0;i<wb;i++)want[k*img+j*row+i]=mem[base+nvn_bl_offset(r.xoffset/bw*bpb+i,r.yoffset/bh+j,is3d?r.zoffset+k:0,rb,rs,th,td)];
        }
        uint64_t reads=atomic_load(&g_texels_gpu_reads),writes=atomic_load(&g_texels_gpu_writes);
        mtl_read_texels(&t,lv,0,&r,got,row,img);
        if(memcmp(got,want,n)||memcmp(mem,expected,size)||atomic_load(&g_texels_gpu_reads)!=reads+(c!=13))MDIE("texels-region production read/guards/coverage case%d",c);
        for(size_t i=0;i<n;i++)src[i]=(uint8_t)(i*19+(i>>3)+31);
        for(int k=0;k<r.depth;k++){
            unsigned layer=is3d?0:r.zoffset+k;uint32_t rb,rs,zs,bpb_;uint64_t base;unsigned th,td;
            nvn_deswizzle_geom(&t.d,lv,layer,&rb,&rs,&zs,&base,&th,&td,&bpb_);
            for(size_t j=0;j<hb;j++)for(size_t i=0;i<wb;i++)expected[base+nvn_bl_offset(r.xoffset/bw*bpb+i,r.yoffset/bh+j,is3d?r.zoffset+k:0,rb,rs,th,td)]=src[k*img+j*row+i];
        }
        nvn_dirty_protect(mem,size);uint64_t stamp=nvn_dirty_now();mtl_write_texels(&t,lv,0,&r,src,row,img);
        if(memcmp(mem,expected,size)||atomic_load(&g_texels_gpu_writes)!=writes+(c!=13)||!nvn_dirty_since(mem,size,stamp))MDIE("texels-region production write case%d data=%d delta=%llu dirty=%d failures=%llu",c,memcmp(mem,expected,size),(unsigned long long)(atomic_load(&g_texels_gpu_writes)-writes),nvn_dirty_since(mem,size,stamp),(unsigned long long)atomic_load(&g_texels_gpu_failures));
        if(c==0){
            /* First Metal sample must see the completed pool write; an already GPU-owned texture must
               keep BOTH its pool mirror and its Metal texture updated by a later WriteTexels. */
            op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);id<MTLTexture> mt=tex_mtl(&t);
            if(!mt)MDIE("texels-region first texture");id<MTLBuffer> out=probe_buf(wb*hb);
            enc_b_copy_texture_buffer(blit(),mt,0,0,MTLOriginMake(r.xoffset,r.yoffset,0),MTLSizeMake(r.width,r.height,1),out,0,wb,wb*hb);
            mtl_sub_commit();mtl_finish();
            for(size_t j=0;j<hb;j++)if(memcmp((uint8_t *)out.contents+j*wb,src+j*row,wb))MDIE("texels-region first GPU sample");
            gpu_owned_mark(t.serial);for(size_t i=0;i<n;i++)src[i]^=0x69;
            mtl_write_texels(&t,lv,0,&r,src,row,img);memset(got,0xa7,n);mtl_read_texels(&t,lv,0,&r,got,row,img);
            for(size_t j=0;j<hb;j++)if(memcmp(got+j*row,src+j*row,wb))MDIE("texels-region GPU-owned Metal mirror");
            uint32_t rb,rs,zs,bpb_;uint64_t base;unsigned th,td;nvn_deswizzle_geom(&t.d,lv,0,&rb,&rs,&zs,&base,&th,&td,&bpb_);
            for(size_t j=0;j<hb;j++)for(size_t i=0;i<wb;i++)expected[base+nvn_bl_offset(r.xoffset*4+i,r.yoffset+j,0,rb,rs,th,td)]=src[j*row+i];
            if(memcmp(mem,expected,size))MDIE("texels-region GPU-owned guest pool mirror");res_remove(out);mtl_tex_release(&t);checked+=2;
        }
        if(c==1){
            /* CPU aliases retain the old operation ordering rather than snapshotting all source rows. */
            uint32_t rb,rs,zs,bpb_;uint64_t base;unsigned th,td;nvn_deswizzle_geom(&t.d,lv,0,&rb,&rs,&zs,&base,&th,&td,&bpb_);
            for(size_t j=0;j<hb;j++)for(size_t i=0;i<wb;i++)expected[base+nvn_bl_offset(r.xoffset*3+i,r.yoffset+j,0,rb,rs,th,td)]=mem[3+j*row+i];
            uint64_t before=atomic_load(&g_texels_gpu_writes);mtl_write_texels(&t,lv,0,&r,mem+3,row,img);
            if(memcmp(mem,expected,size)||atomic_load(&g_texels_gpu_writes)!=before)MDIE("texels-region overlapping source CPU fallback");checked++;
        }
        mtl_pool_del(mem);nvn_dirty_remove_range(mem);mtl_finish();munmap(mem,size);free(expected);free(got);free(want);free(src);checked++;
    }}
    if(atomic_load(&g_texels_gpu_failures))MDIE("texels-region GPU failure");mtl_texels_gpu_report();return checked;
}
