// Production RGBA16F array uploads: compare every component bit, including
// half-float NaN payloads, across mips, faces, reuploads and spare reuse.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull));CHECK("GPU has no submission error",!mtl_last_commit_error()); }
static void run(int target,unsigned width,unsigned height,unsigned layers,unsigned levels,int direct) {
    g_direct_upload_on=direct;
    tex_t t={.magic=MAGIC('T','E','X',0),.has_layout=1};static uint64_t serial=99500;t.serial=serial++;
    t.d=(nvn_texdesc){.target=target,.format=NVN_FORMAT_RGBA16F,.width=width,.height=height,.depth=layers,.levels=levels,.samples=1};nvn_tex_layout(&t.d,&t.L);
    size_t bytes=0;for(unsigned l=0;l<levels;l++)bytes+=(size_t)MAX(width>>l,1u)*MAX(height>>l,1u)*8*layers;
    size_t source_offset=4096+(width==19?4:0),poolsize=(t.L.size+source_offset+getpagesize()-1)&~(size_t)(getpagesize()-1);void *mem=NULL;if(posix_memalign(&mem,getpagesize(),poolsize))abort();memset(mem,0,poolsize);
    pool_t pool={.mem=mem,.size=poolsize,.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};t.pool=&pool;t.mem=(uint8_t*)mem+source_offset;
    mtl_pool_add(mem,poolsize);nvn_dirty_add_range(mem,poolsize);
    uint8_t *expected=malloc(bytes);id<MTLBuffer> readback=[g_dev newBufferWithLength:bytes+512 options:MTLResourceStorageModeShared];id<MTLTexture> first=nil,previous=nil;
    for(unsigned round=0;round<3;round++) {
        dm_guest_write_prepare(mem,poolsize);size_t off=0;
        for(unsigned l=0;l<levels;l++)for(unsigned a=0;a<layers;a++) {
            unsigned w=MAX(width>>l,1u),h=MAX(height>>l,1u);size_t n=(size_t)w*h*8;uint16_t *p=(uint16_t*)(expected+off);
            for(unsigned i=0;i<n/2;i++)p[i]=(uint16_t)(i*137u+a*29u+l*7919u+round*83u);
            if(n>=8){p[0]=0x7e01;p[1]=0x7c00;p[2]=0xfc00;p[3]=0x8000;}
            CHECK("reference swizzle succeeds",!nvn_swizzle(&t.d,l,a,p,w*8,t.mem));off+=n;
        }
        uint64_t uploads=g_direct_uploads,dispatches=g_direct_dispatches;mtl_sub_begin();id<MTLTexture> image=tex_mtl(&t);CHECK("texture resolves",image!=nil);finish();
        int fast=direct&&!(source_offset&7)&&nvn_deswz_gpu_direct_supported(&t.d);CHECK("direct upload selected only when eligible",g_direct_uploads-uploads==(uint64_t)fast);CHECK("one dispatch per mip across all faces",g_direct_dispatches-dispatches==(fast?levels:0));
        if(!round)first=image;else CHECK("pending/read texture is not overwritten",image!=previous);
        if(round==2)CHECK("idle spare texture is reused",image==first);previous=image;
        if(atomic_load(&g_up_lost_n)) {
            uint64_t recovered=atomic_load(&g_up_lost_redo);mtl_sub_begin();image=tex_mtl(&t);finish();
            CHECK("direct target participates in dropped-upload recovery",atomic_load(&g_up_lost_redo)==recovered+1);CHECK("dropped upload marker consumed",!atomic_load(&g_up_lost_n));first=image;previous=image;
        }
        if(!round&&direct){
            tex_t initial=t;initial.has_layout=0;initial.serial+=100000;
            id<MTLTexture> control=[g_dev newTextureWithDescriptor:tex_desc(&initial,0)];res_add(control);uint64_t n=g_direct_uploads;
            mtl_sub_begin();CHECK("initial fill retains its original texture",tex_init_fill(&initial,control));finish();CHECK("initial fill does not require new write/view usage",g_direct_uploads==n);defer_obj_release(control);
        }
        memset(readback.contents,0xcd,readback.length);id<MTLCommandBuffer> cb=q_cb();id<MTLBlitCommandEncoder> ce=[cb blitCommandEncoder];off=0;
        for(unsigned l=0;l<levels;l++)for(unsigned a=0;a<layers;a++) {
            unsigned w=MAX(width>>l,1u),h=MAX(height>>l,1u);[ce copyFromTexture:image sourceSlice:a sourceLevel:l sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:readback destinationOffset:256+off destinationBytesPerRow:w*8 destinationBytesPerImage:w*h*8];off+=(size_t)w*h*8;
        }
        [ce endEncoding];[cb commit];[cb waitUntilCompleted];CHECK("all-mip readback completes",cb.status==MTLCommandBufferStatusCompleted);
        CHECK("all face/mip component bits match CPU reference",!memcmp((uint8_t*)readback.contents+256,expected,bytes));
        CHECK("readback prefix guard",((uint8_t*)readback.contents)[255]==0xcd);CHECK("readback suffix guard",((uint8_t*)readback.contents)[256+bytes]==0xcd);
        uint64_t reuploads=g_pkg_reuploads;mtl_sub_begin();CHECK("unchanged storage reuses its image",tex_mtl(&t)==image);finish();CHECK("unchanged storage is not reuploaded",g_pkg_reuploads==reuploads);
    }
    mtl_tex_release(&t);mtl_drain();nvn_dirty_remove_range(mem);mtl_pool_del(mem);mtl_drain();free(mem);free(expected);
}
int main(void){@autoreleasepool{
    mtl_init();CHECK("direct utility pipeline loads",g_nvn_deswz_gpu&&nvn_deswz_gpu_direct_supported(&(nvn_texdesc){.target=9,.format=NVN_FORMAT_RGBA16F,.width=16,.height=16,.depth=768,.levels=5,.samples=1}));
    nvn_texdesc d={.target=4,.format=NVN_FORMAT_RGBA16F,.width=16,.height=16,.depth=32,.levels=5};
    d.format=NVN_FORMAT_RGBA8;CHECK("other formats stay on the bit-copy path",!nvn_deswz_gpu_direct_supported(&d));d.format=NVN_FORMAT_RGBA16F;d.width=65;CHECK("large images retain compression policy",!nvn_deswz_gpu_direct_supported(&d));d.width=16;d.flags=NVN_TEXTURE_FLAGS_LINEAR;CHECK("linear images are excluded",!nvn_deswz_gpu_direct_supported(&d));
    for(int direct=0;direct<2;direct++){run(4,17,9,37,5,direct);run(9,16,16,768,5,direct);run(4,65,3,32,4,direct);run(4,19,9,37,5,direct);}
    printf("t_direct_upload: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
