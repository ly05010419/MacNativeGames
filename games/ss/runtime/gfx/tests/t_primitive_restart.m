// Indexed fans and quads keep restart-separated polygons apart on the real GPU.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c,label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while(0)
int main(void){@autoreleasepool{
    mtl_init();NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]){const float2 p[8]={float2(-.9,-.8),float2(-.3,-.8),float2(-.3,.8),float2(-.9,.8),float2(.3,-.8),float2(.9,-.8),float2(.9,.8),float2(.3,.8)};return float4(p[i%8],.5,1);}\n"
        "fragment float4 f(){return float4(1,0,0,1);}\n" options:nil error:&error];
    CHECK(lib!=nil,"synthetic shaders compile");if(!lib)return 1;
    NvProg *pr=[NvProg new];pr->sh[0]=[NvShader new];pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"];pr->sh[1]->fn=[lib newFunctionWithName:@"f"];pr->sh[1]->stage=1;
    pr->sh[1]->rf.color_known=1;pr->sh[1]->rf.color_mask=1;
    prog_t pg={.serial=99501};serial_put(pg.serial,pr);
    size_t page=(size_t)getpagesize();void *mem=NULL;if(posix_memalign(&mem,page,page))return 2;mtl_pool_add(mem,page);
    int test=0;
    for(int isz=1;isz<=4;isz*=2)for(int custom=0;custom<2;custom++)for(int prim=6;prim<=7;prim++){
        uint32_t restart=custom?13:isz==1?255:isz==2?65535:UINT32_MAX;
        uint32_t indices[]={restart,0,1,restart,restart,0,1,2,3,restart,4,5,6,7,restart};
        for(unsigned i=0;i<sizeof indices/sizeof *indices;i++)memcpy((uint8_t*)mem+i*isz,&indices[i],isz);
        tex_t t={.magic=MAGIC('T','E','X',0),.serial=99600+test++};
        t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=32,.height=16,.depth=1,.levels=1,.samples=1};
        MTLTextureDescriptor *td=tex_desc(&t,0);td.storageMode=MTLStorageModeShared;id<MTLTexture>out=[g_dev newTextureWithDescriptor:td];res_add(out);mtl_tex_init(&t);serial_put(t.serial,out);
        state_reset();memset(&R,0,sizeof R);mtl_sub_begin();R.has_rt=1;R.rt.n=1;R.rt.color[0]=&t;R.clear_c[0]=1;R.cc[0]=MTLClearColorMake(0,0,0,0);
        S.prog=&pg;S.ds.dt=S.ds.dw=S.ds.st=0;S.poly.cull=0;S.color.blend_en=0;S.chmask.mask=0xffff;S.restart=(p_prim_restart){1,restart};
        p_draw_idx di={.prim=prim,.count=sizeof indices/sizeof *indices,.itype=isz==1?0:isz==2?1:2,.ibuf=(uint64_t)mem,.instances=1};draw(NULL,&di);
        uint64_t fence=mtl_sub_commit();CHECK(mtl_wait(fence,5000000000ull),"restart draw completes");CHECK(!mtl_last_commit_error(),"restart draw has no GPU error");
        uint8_t pixels[32*16*4];[out getBytes:pixels bytesPerRow:32*4 fromRegion:MTLRegionMake2D(0,0,32,16) mipmapLevel:0];
        int good=1;for(int y=3;y<13;y++)for(int x=0;x<32;x++){int lit=(x>=2&&x<11)||(x>=21&&x<30);good&=(pixels[(y*32+x)*4]>127)==lit;}
        if (!good) { fprintf(stderr,"row8:"); for(int x=0;x<32;x++) fprintf(stderr,"%c",pixels[(8*32+x)*4]>127?'#':'.'); fputc('\n',stderr); }
        CHECK(good,"two polygons lit, gap and outside untouched");printf("indices=%d custom=%d prim=%d %s\n",isz,custom,prim,good?"PASS":"FAIL");mtl_tex_release(&t);
    }
    mtl_pool_del(mem);mtl_drain();free(mem);printf("primitive restart: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
