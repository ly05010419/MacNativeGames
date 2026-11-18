// CPU eligibility counterexample: real production rt_range_same, Metal objects supplied by explicit mocks; no GPU is started and no pixels are proven.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void){return NULL;}
module_t *module_of(const void *p){(void)p;return NULL;}
module_t g_mod[MAX_MOD];int g_nmod;void svc_thread_dump(void){}uint64_t now_ns(void){return bp_now();}
@interface MockTex:NSObject @end
@implementation MockTex
-(NSUInteger)width{return 8;}-(NSUInteger)height{return 8;}-(NSUInteger)sampleCount{return 1;}-(MTLTextureType)textureType{return MTLTextureType2D;}
@end
static int n,bad;
#define E(s,w,a) do{int v=(a);n++;printf("%s expected %d → got %d\n",s,w,v);bad+=(w)!=v;}while(0)
static void history(void){
#ifdef NVN_RT_RANGE_DEPTH_IMPL
 rt_range_note_depth();
#endif
}
int main(void){@autoreleasepool{setenv("DM_NVN_RT_RANGE","1",1);tex_t c={.magic=MAGIC('T', 'E', 'X', 0), .serial =26},d={.magic=MAGIC('T', 'E', 'X', 0), .serial =29};c.d.width=c.d.height=d.d.width=d.d.height=8;c.d.depth=d.d.depth=c.d.samples=d.d.samples=1;c.d.levels=4;d.d.levels=1;c.d.target=d.d.target=NVN_TEXTURE_TARGET_2D;c.d.format=NVN_FORMAT_RGBA8;d.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
 view_t cv={.set_levels=1,.nlevels=4};p_set_rt a={.n=1,.depth=&d};a.color[0]=&c;a.has_cview[0]=1;a.cview[0]=cv;p_set_rt b=a;b.cview[0].nlevels=1;
 id<MTLTexture> tc=(id)[MockTex new],td=(id)[MockTex new];serial_put(26,tc);serial_put(29,td);g_enc=(id)[NSObject new];g_enc_mode=4;g_pass.hd=1;g_pass.tw=g_pass.th=8;g_pass.range_color=tc;g_pass.range_depth=td;g_pass.reg_gen=g_reg_gen;g_pass.ser_gen=g_ser_gen;g_pass.range_dirty=nvn_dirty_now();S.ds.dt=1;
 E("previously proven depth-RO eligibility kept",1,rt_range_same(&a,&b));S.ds.dw=1;E("current depth write rejected",0,rt_range_same(&a,&b));history();S.ds.dw=0;E("earlier depth write: final state alone is not enough",0,rt_range_same(&a,&b));
#ifdef NVN_RT_RANGE_DEPTH_IMPL
 g_pass.range_depth_nonro=0;
#endif
 S.ds.dt=0;E("no current depth read does not widen the checker's reach",0,rt_range_same(&a,&b));g_enc=nil;printf("RT_DEPTH_RO cases=%d failed=%d MOCK_METAL_NO_GPU\n",n,bad);return bad!=0;}}
